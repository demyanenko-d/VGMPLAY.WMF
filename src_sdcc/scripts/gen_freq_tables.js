/**
 * gen_freq_tables.js — Генерация LUT таблиц масштабирования частот PSG
 *
 * Использование:
 *   node scripts/gen_freq_tables.js [--binary <output.bin>] [--asm <output.s>] [--info]
 *
 * Каждая таблица: 4096 entries × uint16_t LE = 8192 байт (8 КБ).
 *
 * Покрывает:
 *   - AY/YM2149: 12-bit tone period (regs 0x00–0x05), 0..4095
 *   - Noise: 5-bit (reg 0x06), 0..31 — первые 32 записи
 *
 * Формула масштабирования PSG:
 *   F = CLK / (16 × period)   → scaled = period × (target / source)
 *
 * FM F-Number масштабирование (YM2203) требует ОБРАТНОГО ratio (source/target)
 * и выполняется runtime через дробные коэффициенты (fm_frac_t в C-хедере).
 *
 * Layout: 2 ratios per 16 КБ page:
 *   0x0000–0x1FFF : PSG таблица A (8 КБ)
 *   0x2000–0x3FFF : PSG таблица B (8 КБ)
 *
 * Итого: 3 pages × 16 КБ = 48 КБ (3 доп. страницы)
 */

'use strict';

const fs   = require('fs');
const path = require('path');

/* ── Целевые частоты нашего железа (TSConfig MultiSound) ─────────────── */
const HW_CLK_AY     = 1750000;  /* AY/YM2149: 1.75 МГц */
const HW_CLK_YM2203 = 3500000;  /* YM2203 OPN: 3.5 МГц */

/* ── Порог округления при сравнении частот ────────────────────────────
 * Если VGM clock отличается от канонического ≤ MATCH_TOLERANCE_PCT,
 * считаем что это тот же коэффициент. Отдельно: если ratio ≈ 1.0
 * (±BYPASS_PCT), масштабирование не нужно вообще. */
const MATCH_TOLERANCE_PCT = 5.0;   /* % — порог совпадения таблиц   */
const BYPASS_PCT          = 2.0;   /* % — порог bypass (≈1:1)       */

/* ── Предопределённые варианты VGM-клоков ─────────────────────────────
 * Для YM2203: PSG (SSG) = fclk/2, FM = fclk
 * Для AY8910: PSG = fclk
 *
 * PSG ratio = HW_CLK_AY / srcPsgHz  (target/source → period scaling)
 * FM  ratio = srcYM2203 / HW_CLK_YM2203  (source/target → fnum scaling, INVERSE!)
 *
 * PSG ratio и FM ratio НЕ равны друг другу — нужны ОТДЕЛЬНЫЕ таблицы!
 *   PSG: scaled_period = period × (HW_CLK_AY / srcPsgHz)
 *   FM:  scaled_fnum   = fnum   × (srcYM2203 / HW_CLK_YM2203)
 *
 * FM масштабирование: все используемые FM ratios сводятся к N/7,
 * поэтому fm_scale хранит множитель N (0=bypass, 3, 5, 6, 8).
 * В ASM на Z80: fnum × N через shifts+adds, затем /7 binary division.
 * NTSC (45/44 ≈ 1.023) — bypass, погрешность 2.3% в рамках 5% допуска.
 *
 * Близкие частоты (≤ MATCH_TOLERANCE_PCT) объединяются:
 *   1996800 Hz ≈ 2000000 Hz (0.16%) → используют одну таблицу '4MHz_2000'
 *   3993600 Hz ≈ 4000000 Hz (0.16%) → то же самое
 */
const RATIOS = [
    {
        name: 'NTSC_1790',
        srcPsgHz:  1789773,  /* AY8910 @ 1789773 или YM2203 @ 3579545 (SSG=1789773) */
        srcYM2203: 3579545,
        fm_scale: 0,  /* 45/44 ≈ 1.023 — bypass (2.3% в рамках 5% допуска) */
        desc: 'NTSC-derived (NES, many JP computers)',
    },
    {
        name: '3MHz_1500',
        srcPsgHz:  1500000,  /* AY8910 @ 1.5M или YM2203 @ 3M */
        srcYM2203: 3000000,
        fm_scale: 6,  /* 6/7 — fnum × 6 / 7 */
        desc: 'PC-88, MSX (3 MHz YM2203)',
    },
    {
        name: '4MHz_2000',
        srcPsgHz:  2000000,  /* AY8910 @ 2M или YM2203 @ 4M (≈1996800/3993600) */
        srcYM2203: 4000000,
        fm_scale: 8,  /* 8/7 — fnum × 8 / 7 */
        desc: 'Arcade boards (4 MHz YM2203, covers 3.9936 MHz)',
    },
    {
        name: 'CPC_1000',
        srcPsgHz:  1000000,  /* Amstrad CPC AY @ 1 MHz */
        srcYM2203: 0,        /* нет FM */
        desc: 'Amstrad CPC (1 MHz AY)',
    },
    {
        name: '1250',
        srcPsgHz:  1250000,
        srcYM2203: 2500000,
        fm_scale: 5,  /* 5/7 — fnum × 5 / 7 */
        desc: 'Some boards (1.25 MHz AY / 2.5 MHz YM2203)',
    },
    {
        name: '1500_750',
        srcPsgHz:   750000,  /* YM2203 @ 1.5M → SSG = 750 kHz */
        srcYM2203: 1500000,
        fm_scale: 3,  /* 3/7 — fnum × 3 / 7 */
        desc: 'Arcade/custom boards (1.5 MHz YM2203, SSG=750 kHz)',
    },
];

/**
 * matchTable(psgClkHz) → { index, ratio, bypass }
 *
 * Ищет ближайшую таблицу для заданного PSG clock.
 * Если расхождение ratio от 1.0 ≤ BYPASS_PCT → bypass (масштабирование не нужно).
 * Если ближайшая таблица ≤ MATCH_TOLERANCE_PCT → возвращает её индекс.
 * Иначе → index = -1 (нужна runtime-генерация или таблицы нет).
 */
function matchTable(psgClkHz) {
    const ratio = psgClkHz / HW_CLK_AY;

    /* Bypass: частоты почти совпадают с HW */
    if (Math.abs(ratio - 1.0) * 100 <= BYPASS_PCT) {
        return { index: -1, ratio, bypass: true, name: 'BYPASS' };
    }

    let bestIdx = -1;
    let bestDiffPct = Infinity;

    for (let i = 0; i < RATIOS.length; i++) {
        const diffPct = Math.abs(psgClkHz - RATIOS[i].srcPsgHz) / RATIOS[i].srcPsgHz * 100;
        if (diffPct < bestDiffPct) {
            bestDiffPct = diffPct;
            bestIdx = i;
        }
    }

    if (bestDiffPct <= MATCH_TOLERANCE_PCT) {
        return {
            index: bestIdx,
            ratio: RATIOS[bestIdx].srcPsgHz / HW_CLK_AY,
            bypass: false,
            name: RATIOS[bestIdx].name,
            diffPct: bestDiffPct,
        };
    }

    /* Нет подходящей таблицы — runtime generation needed */
    return { index: -1, ratio, bypass: false, name: 'RUNTIME' };
}

const TABLE_ENTRIES = 4096;  /* 12-bit PSG range */
const TABLE_BYTES  = TABLE_ENTRIES * 2;  /* uint16_t LE = 8 КБ */
const PAGE_SIZE    = 16384;  /* 16 КБ */

/* ── Генерация PSG таблицы ───────────────────────────────────────────
 * ratio = target / source (period scales up/down to preserve frequency)
 * PSG: F = CLK / (16 × period)  →  scaled_period = period × (target/source)
 */
function generateTable(srcPsgHz, targetHz) {
    const buf = Buffer.alloc(TABLE_BYTES);
    const ratio = targetHz / srcPsgHz;

    for (let i = 0; i < TABLE_ENTRIES; i++) {
        let scaled = Math.round(i * ratio);
        if (scaled > 0xFFFF) scaled = 0xFFFF;  /* clamp uint16 */
        buf.writeUInt16LE(scaled, i * 2);
    }

    return buf;
}

/* ── Вывод информации о таблицах ─────────────────────────────────────── */
function printInfo() {
    console.log('=== Frequency Scale LUT Tables ===');
    console.log(`Target PSG: ${HW_CLK_AY} Hz`);
    console.log(`PSG table: ${TABLE_ENTRIES} entries × 2 bytes = ${TABLE_BYTES} bytes`);
    console.log(`Layout: 2 ratios per 16 КБ page`);
    console.log(`Total: ${Math.ceil(RATIOS.length / 2)} pages × 16 КБ = ${Math.ceil(RATIOS.length / 2) * PAGE_SIZE} bytes`);
    console.log('');

    for (let r = 0; r < RATIOS.length; r++) {
        const rt = RATIOS[r];
        const psgRatio = HW_CLK_AY / rt.srcPsgHz;
        const page  = Math.floor(r / 2) + PAGE_BASE;
        const slot  = r % 2;
        const offset = slot * TABLE_BYTES;

        console.log(`[${r}] ${rt.name}: PSG ${rt.srcPsgHz} Hz → ratio = ${psgRatio.toFixed(6)}`);
        console.log(`    ${rt.desc}`);
        console.log(`    Page ${page}, slot ${slot} (offset 0x${offset.toString(16).toUpperCase()})`);
        if (rt.srcYM2203) {
            const fmRatio = rt.srcYM2203 / HW_CLK_YM2203;
            const fmDesc = rt.fm_scale ? `×${rt.fm_scale}/7` : 'bypass';
            console.log(`    FM: YM2203 @ ${rt.srcYM2203} Hz → fm_ratio = ${fmRatio.toFixed(6)} (${fmDesc})`);
        }

        /* Проверка нескольких ключевых значений */
        const checks = [
            { name: 'PSG tone 440 Hz equiv', val: Math.round(rt.srcPsgHz / (16 * 440)) },
            { name: 'PSG max 12-bit',        val: 4095 },
            { name: 'Noise max 5-bit',       val: 31 },
        ];
        for (const c of checks) {
            if (c.val >= TABLE_ENTRIES) continue;
            const scaled = Math.round(c.val * psgRatio);
            console.log(`    PSG ${c.name}: ${c.val} → ${scaled}`);
        }
        console.log('');
    }

    /* ── Тест: сопоставление «диких» частот из реальных VGM ────────── */
    console.log('--- Clock matching test (bypass ' + BYPASS_PCT + '%, tolerance ' + MATCH_TOLERANCE_PCT + '%) ---');
    const testClocks = [
        /* PSG clocks (AY8910 fclk, или YM2203 SSG = fclk/2) */
        { name: 'TSconf HW (bypass)',      hz: 1750000 },
        { name: 'Close to HW (bypass)',     hz: 1773400 },
        { name: 'NTSC exact',              hz: 1789773 },
        { name: 'NTSC rounded',            hz: 1789750 },
        { name: 'YM2203 3.58MHz SSG',      hz: 1789772 },
        { name: 'PAL-derived',             hz: 1773448 },
        { name: 'MSX 3MHz SSG',            hz: 1500000 },
        { name: 'MSX variant',             hz: 1536000 },
        { name: '4MHz SSG exact',          hz: 2000000 },
        { name: '3.9936MHz SSG',           hz: 1996800 },
        { name: '4.028MHz SSG',            hz: 2014000 },
        { name: 'CPC exact',               hz: 1000000 },
        { name: 'CPC-ish',                 hz: 1023000 },
        { name: '2.5MHz SSG',              hz: 1250000 },
        { name: 'Weird 1.3MHz',            hz: 1300000 },
        { name: 'Weird 1.6MHz',            hz: 1600000 },
        { name: 'Weird 900kHz',            hz: 900000 },
        { name: 'YM2203 1.5MHz SSG',       hz: 750000 },
        { name: 'ZX 128 (1.7734 MHz)',     hz: 1773400 },
    ];
    for (const t of testClocks) {
        const m = matchTable(t.hz);
        const tag = m.bypass ? 'BYPASS' :
                    m.index >= 0 ? `table[${m.index}]=${m.name} (${m.diffPct.toFixed(2)}%)` :
                    'NO MATCH → runtime';
        console.log(`  ${t.hz.toString().padStart(7)} Hz  ${t.name.padEnd(26)} → ${tag}`);
    }

    /* Подсказка по Z80 маппингу */
    console.log('');
    console.log('--- Z80 matching: compare clock_khz ±5% ---');
    for (let r = 0; r < RATIOS.length; r++) {
        const khz = Math.round(RATIOS[r].srcPsgHz / 1000);
        const tol = Math.round(khz * MATCH_TOLERANCE_PCT / 100);
        const page = Math.floor(r / 2) + PAGE_BASE;
        const slot = r % 2;
        console.log(`  [${r}] ${RATIOS[r].name.padEnd(12)} ${khz} kHz ± ${tol} kHz  →  page ${page}, slot ${slot}`);
    }
}

/* ── Генерация бинарного файла ───────────────────────────────────────── *
 * Layout: 2 ratios per 16 КБ page.
 *
 *   Page N+0: PSG[0] (0x0000-0x1FFF) + PSG[1] (0x2000-0x3FFF)
 *   Page N+1: PSG[2] (0x0000-0x1FFF) + PSG[3] (0x2000-0x3FFF)
 *   ...
 *
 * Итого: ceil(RATIOS.length / 2) × 16 КБ
 */
function generateBinary(outPath) {
    const numPages = Math.ceil(RATIOS.length / 2);
    const totalSize = numPages * PAGE_SIZE;
    const out = Buffer.alloc(totalSize, 0x00);

    for (let r = 0; r < RATIOS.length; r++) {
        const page = Math.floor(r / 2);
        const slot = r % 2;
        const offset = page * PAGE_SIZE + slot * TABLE_BYTES;

        const table = generateTable(RATIOS[r].srcPsgHz, HW_CLK_AY);
        table.copy(out, offset);
    }

    fs.writeFileSync(outPath, out);
    console.log(`Written: ${outPath} (${out.length} bytes, ${numPages} pages)`);

    /* Вывод маппинга для кода */
    console.log('\nC mapping (for vgm.c):');
    console.log('// After determining VGM PSG clock, select table:');
    for (let r = 0; r < RATIOS.length; r++) {
        const page = Math.floor(r / 2) + PAGE_BASE;
        const offset = (r % 2) * TABLE_BYTES;
        console.log(`//   ${RATIOS[r].srcPsgHz} Hz → wc_mng0_pl(${page}), offset = 0x${offset.toString(16)}`);
    }
}

/* ── Генерация ASM include (для верификации) ─────────────────────────── */
function generateAsm(outPath) {
    let asm = '; Auto-generated by gen_freq_tables.js\n';
    asm += '; Frequency scale LUT tables for PSG/FM\n';
    asm += `; Target: PSG=${HW_CLK_AY} Hz, FM=${HW_CLK_YM2203} Hz\n`;
    asm += `; ${TABLE_ENTRIES} entries × 2 bytes = ${TABLE_BYTES} bytes per table\n\n`;

    for (let r = 0; r < RATIOS.length; r++) {
        const rt = RATIOS[r];
        const ratio = rt.srcPsgHz / HW_CLK_AY;
        asm += `; Table ${r}: ${rt.name} (${rt.srcPsgHz} Hz, ratio=${ratio.toFixed(6)})\n`;
        asm += `_freq_lut_${r}::\n`;

        const table = generateTable(rt.srcPsgHz, HW_CLK_AY);
        for (let i = 0; i < TABLE_ENTRIES; i += 8) {
            const values = [];
            for (let j = 0; j < 8 && (i + j) < TABLE_ENTRIES; j++) {
                values.push('0x' + table.readUInt16LE((i + j) * 2).toString(16).padStart(4, '0'));
            }
            asm += `    .dw ${values.join(', ')}\n`;
        }
        asm += '\n';
    }

    fs.writeFileSync(outPath, asm);
    console.log(`Written: ${outPath}`);
}

/* ── Генерация C-хедера для Z80 маппинга ─────────────────────────────
 * Содержит таблицу {clk_khz, tol_khz, page, offset} для линейного
 * поиска ближайшей LUT при парсинге VGM заголовка.
 *
 * Layout: 2 ratios per page — PSG_A@0x0000 (8 КБ) + PSG_B@0x2000 (8 КБ).
 * FM масштабирование — runtime через fm_frac_t (дробные коэффициенты).
 *
 * Алгоритм на Z80 (в vgm.c):
 *   1. bypass: |psg_khz - 1750| ≤ 35 → нет масштабирования
 *   2. Линейный поиск: |psg_khz - entry.clk_khz| ≤ entry.tol_khz → match
 *   3. Нет match → native (без масштабирования)
 *
 * Все сравнения — 16 бит (uint16_t kHz), дёшево на Z80.
 */
function generateCHeader(outPath) {
    let h = '';
    h += '/* Auto-generated by gen_freq_tables.js — DO NOT EDIT */\n';
    h += '#ifndef FREQ_LUT_MAP_H\n';
    h += '#define FREQ_LUT_MAP_H\n\n';
    h += '#include "types.h"\n\n';

    /* Count YM2203 entries (those with srcYM2203 > 0) */
    const ymEntries = RATIOS.filter(r => r.srcYM2203 > 0);

    h += `/* AY/YM2149: bypass |psg_khz - ${Math.round(HW_CLK_AY/1000)}| <= ${Math.round(HW_CLK_AY/1000 * BYPASS_PCT / 100)} */\n`;
    h += `#define FREQ_LUT_HW_KHZ        ${Math.round(HW_CLK_AY/1000)}u\n`;
    h += `#define FREQ_LUT_BYPASS_TOL     ${Math.round(HW_CLK_AY/1000 * BYPASS_PCT / 100)}u  /* ${BYPASS_PCT}% */\n`;
    h += `/* YM2203: bypass |ym_khz - ${Math.round(HW_CLK_YM2203/1000)}| <= ${Math.round(HW_CLK_YM2203/1000 * BYPASS_PCT / 100)} */\n`;
    h += `#define FREQ_LUT_HW_YM_KHZ     ${Math.round(HW_CLK_YM2203/1000)}u\n`;
    h += `#define FREQ_LUT_BYPASS_TOL_YM  ${Math.round(HW_CLK_YM2203/1000 * BYPASS_PCT / 100)}u  /* ${BYPASS_PCT}% */\n\n`;

    h += `#define FREQ_LUT_AY_COUNT     ${RATIOS.length}u\n`;
    h += `#define FREQ_LUT_YM_COUNT     ${ymEntries.length}u\n`;
    h += `#define FREQ_LUT_PSG_ENTRIES  ${TABLE_ENTRIES}u   /* 12-bit PSG */\n`;
    h += `#define FREQ_LUT_PSG_BYTES    ${TABLE_BYTES}u     /* per PSG table */\n\n`;

    h += 'typedef struct {\n';
    h += '    uint16_t clk_khz;   /* canonical clock in kHz (PSG or YM master) */\n';
    h += '    uint16_t tol_khz;   /* \u00b1tolerance in kHz (5%)      */\n';
    h += '    uint8_t  page;      /* plugin page (PAGE_BASE-based) */\n';
    h += '    uint16_t offset;    /* PSG table byte offset within page */\n';
    h += '} freq_lut_entry_t;\n\n';



    /* AY map — PSG only */
    h += `/* AY/YM2149 PSG table map (tolerance = ${MATCH_TOLERANCE_PCT}% of canonical PSG clock)\n`;
    h += ` * Layout: 2 ratios per page, PSG_A@0x0000, PSG_B@0x${TABLE_BYTES.toString(16).toUpperCase()} */\n`;
    h += 'static const freq_lut_entry_t freq_lut_map_ay[FREQ_LUT_AY_COUNT] = {\n';

    for (let r = 0; r < RATIOS.length; r++) {
        const khz = Math.round(RATIOS[r].srcPsgHz / 1000);
        const tol = Math.round(khz * MATCH_TOLERANCE_PCT / 100);
        const page = Math.floor(r / 2) + PAGE_BASE;
        const offset = (r % 2) * TABLE_BYTES;
        const comma = (r < RATIOS.length - 1) ? ',' : ' ';
        h += `    { ${khz.toString().padStart(4)}u, ${tol.toString().padStart(3)}u, ${page}, 0x${offset.toString(16).toUpperCase().padStart(4, '0')} }${comma}  /* ${RATIOS[r].name} */\n`;
    }
    h += '};\n\n';

    /* YM2203 map — uses same PSG table as AY, plus FM frac for runtime */
    h += '/* YM2203 table map (full master clock, PSG table = same as AY entry) */\n';
    h += 'static const freq_lut_entry_t freq_lut_map_ym[FREQ_LUT_YM_COUNT] = {\n';

    let ymIdx = 0;
    for (let r = 0; r < RATIOS.length; r++) {
        if (!RATIOS[r].srcYM2203) continue;
        const khz = Math.round(RATIOS[r].srcYM2203 / 1000);
        const tol = Math.round(khz * MATCH_TOLERANCE_PCT / 100);
        const page = Math.floor(r / 2) + PAGE_BASE;
        const offset = (r % 2) * TABLE_BYTES;
        ymIdx++;
        const comma = (ymIdx < ymEntries.length) ? ',' : ' ';
        h += `    { ${khz.toString().padStart(4)}u, ${tol.toString().padStart(3)}u, ${page}, 0x${offset.toString(16).toUpperCase().padStart(4, '0')} }${comma}  /* ${RATIOS[r].name} (YM2203@${khz}kHz) */\n`;
    }
    h += '};\n\n';

    /* FM multiplier table — indexed same as freq_lut_map_ym[] */
    h += '/* FM F-Number \u043c\u043d\u043e\u0436\u0438\u0442\u0435\u043b\u044c (YM2203): scaled = fnum \u00d7 fm_mul / 7.\n';
    h += ' * 0 = bypass (\u043c\u0430\u0441\u0448\u0442\u0430\u0431\u0438\u0440\u043e\u0432\u0430\u043d\u0438\u0435 \u043d\u0435 \u043d\u0443\u0436\u043d\u043e).\n';
    h += ' * ASM \u043d\u0430 Z80: \u0443\u043c\u043d\u043e\u0436\u0435\u043d\u0438\u0435 \u0447\u0435\u0440\u0435\u0437 shifts+adds, \u0434\u0435\u043b\u0435\u043d\u0438\u0435 \u043d\u0430 7 binary division */\n';
    h += 'static const uint8_t freq_lut_fm_mul[FREQ_LUT_YM_COUNT] = {\n';

    ymIdx = 0;
    for (let r = 0; r < RATIOS.length; r++) {
        if (!RATIOS[r].srcYM2203) continue;
        const rt = RATIOS[r];
        const fmRatio = rt.srcYM2203 / HW_CLK_YM2203;
        ymIdx++;
        const comma = (ymIdx < ymEntries.length) ? ',' : ' ';
        const fmDesc = rt.fm_scale ? `\u00d7${rt.fm_scale}/7` : 'bypass (2.3%)';
        h += `    ${rt.fm_scale}${comma}  /* ${rt.name}: ${fmRatio.toFixed(6)} = ${fmDesc} */\n`;
    }
    h += '};\n\n';

    h += '/*\n';
    h += ' * Usage in vgm.c (parse_header):\n';
    h += ' *\n';
    h += ' *   1. Bypass check: |clock_khz - HW| <= BYPASS_TOL \u2192 native\n';
    h += ' *   2. Find matching table \u2192 set page, PSG base = offset\n';
    h += ' *   3. For YM2203: fm_mul = freq_lut_fm_mul[j]\n';
    h += ' *\n';
    h += ' *   PSG: scaled_period = freq_lut_base[period_12bit]\n';
    h += ' *   FM:  asm_scale_fm() : fb_scaled = fnum \u00d7 fm_mul / 7\n';
    h += ' *        if (fb_scaled > 2047) block++, fb_scaled >>= 1\n';
    h += ' */\n\n';

    h += '#endif /* FREQ_LUT_MAP_H */\n';

    fs.writeFileSync(outPath, h);
    console.log(`Written: ${outPath}`);
}

/* ── Номер первой plugin-страницы с LUT данными ─────────────────────
 * Зависит от числа страниц кода: code_pages = ceil(code_size / 16384).
 * Передаётся через --page-base N.  По умолчанию 2 (типичный случай:
 * CODE+DATA ≈ 15-16 КБ, занимает 2 страницы #8000-#FFFF). */
let PAGE_BASE = 2;

/* ── CLI ─────────────────────────────────────────────────────────────── */
function main() {
    const args = process.argv.slice(2);
    let doBinary = null;
    let doAsm = null;
    let doCheader = null;
    let doInfo = false;

    for (let i = 0; i < args.length; i++) {
        if (args[i] === '--binary' && args[i + 1])  { doBinary = args[++i]; }
        else if (args[i] === '--asm' && args[i + 1]) { doAsm = args[++i]; }
        else if (args[i] === '--cheader' && args[i + 1]) { doCheader = args[++i]; }
        else if (args[i] === '--page-base' && args[i + 1]) { PAGE_BASE = parseInt(args[++i], 10); }
        else if (args[i] === '--info')                { doInfo = true; }
        else {
            console.error(`Unknown arg: ${args[i]}`);
            console.error('Usage: node gen_freq_tables.js [--binary out.bin] [--asm out.s] [--cheader out.h] [--page-base N] [--info]');
            process.exit(1);
        }
    }

    if (!doBinary && !doAsm && !doCheader && !doInfo) {
        doInfo = true;  /* default: print info */
    }

    if (doInfo)    printInfo();
    if (doBinary)  generateBinary(doBinary);
    if (doAsm)     generateAsm(doAsm);
    if (doCheader) generateCHeader(doCheader);
}

main();

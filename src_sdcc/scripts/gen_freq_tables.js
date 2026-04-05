/**
 * gen_freq_tables.js — Генерация LUT таблиц масштабирования частот PSG/FM
 *
 * Использование:
 *   node scripts/gen_freq_tables.js [--binary <output.bin>] [--asm <output.s>] [--info]
 *
 * Таблица: 4096 entries × uint16_t LE = 8192 байт (8 КБ) на коэффициент.
 *
 * Покрывает:
 *   - PSG (AY/YM2149): 12-bit tone period (regs 0x00–0x05), 0..4095
 *   - PSG noise: 5-bit (reg 0x06), 0..31 — первые 32 записи
 *
 * Формула: table[i] = round(i × f_target / f_source)
 *   Для PSG: f_source = VGM PSG clock, f_target = 1750000 Hz
 *   Период УМЕНЬШАЕТСЯ при target < source → частота сохраняется.
 *
 * FM F-Number масштабирование требует ОБРАТНОГО ratio (source/target)
 * и реализуется отдельно (TODO: shadow-регистры FM каналов).
 *
 * При использовании build-time подхода (6 таблиц в WMF):
 *   --binary freq_tables.bin  → 6 × 8 КБ = 48 КБ raw data
 *   Расположение: по 2 таблицы на 16 КБ страницу (3 доп. страницы)
 *
 * При runtime генерации на Z80 (~300 мс):
 *   Этот скрипт используется для верификации / тестирования.
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
 * Для YM2203: PSG = fclk/2, FM = fclk
 * Для AY8910: PSG = fclk
 *
 * ratio = f_source_psg / HW_CLK_AY = f_source_ym2203 / HW_CLK_YM2203
 * (одинаков для PSG и FM части одного и того же чипа)
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
        desc: 'NTSC-derived (NES, many JP computers)',
    },
    {
        name: '3MHz_1500',
        srcPsgHz:  1500000,  /* AY8910 @ 1.5M или YM2203 @ 3M */
        srcYM2203: 3000000,
        desc: 'PC-88, MSX (3 MHz YM2203)',
    },
    {
        name: '4MHz_2000',
        srcPsgHz:  2000000,  /* AY8910 @ 2M или YM2203 @ 4M (≈1996800/3993600) */
        srcYM2203: 4000000,
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
        desc: 'Some boards (1.25 MHz AY / 2.5 MHz YM2203)',
    },
    {
        name: '1500_750',
        srcPsgHz:   750000,  /* YM2203 @ 1.5M → SSG = 750 kHz */
        srcYM2203: 1500000,
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
const TABLE_BYTES  = TABLE_ENTRIES * 2;  /* uint16_t LE */
const PAGE_SIZE    = 16384;  /* 16 КБ */

/* ── Генерация одной таблицы ─────────────────────────────────────────── */
function generateTable(srcPsgHz, targetHz) {
    const buf = Buffer.alloc(TABLE_BYTES);
    const ratio = targetHz / srcPsgHz;  /* PSG: period shrinks when target < source */

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
    console.log(`Target PSG: ${HW_CLK_AY} Hz, Target FM: ${HW_CLK_YM2203} Hz`);
    console.log(`Table: ${TABLE_ENTRIES} entries × 2 bytes = ${TABLE_BYTES} bytes per ratio`);
    console.log(`Total: ${RATIOS.length} tables × ${TABLE_BYTES} = ${RATIOS.length * TABLE_BYTES} bytes`);
    console.log(`Pages: ${Math.ceil(RATIOS.length * TABLE_BYTES / PAGE_SIZE)} × 16 КБ`);
    console.log(`  (2 tables per page)`);
    console.log('');

    for (let r = 0; r < RATIOS.length; r++) {
        const rt = RATIOS[r];
        const ratio = HW_CLK_AY / rt.srcPsgHz;  /* same as generateTable: target/source */
        const page  = Math.floor(r / 2) + PAGE_BASE; /* plugin page (PAGE_BASE = after code pages) */
        const slot  = r % 2;                          /* 0 or 1 within page */
        const base  = slot * TABLE_BYTES;           /* byte offset within page */

        console.log(`[${r}] ${rt.name}: ${rt.srcPsgHz} Hz → ratio = ${ratio.toFixed(6)}`);
        console.log(`    ${rt.desc}`);
        console.log(`    Page ${page}, offset 0x${base.toString(16).toUpperCase()} (slot ${slot})`);
        if (rt.srcYM2203) {
            console.log(`    FM: YM2203 @ ${rt.srcYM2203} Hz → same ratio`);
        }

        /* Проверка нескольких ключевых значений */
        const checks = [
            { name: 'PSG tone 440 Hz equiv', val: Math.round(rt.srcPsgHz / (16 * 440)) },
            { name: 'FM F-Num mid',          val: 1024 },
            { name: 'PSG max 12-bit',        val: 4095 },
            { name: 'Noise max 5-bit',       val: 31 },
        ];
        for (const c of checks) {
            if (c.val >= TABLE_ENTRIES) continue;
            const scaled = Math.round(c.val * ratio);
            console.log(`    ${c.name}: ${c.val} → ${scaled}` +
                        (c.val <= 2047 ? ` (FM OK: ${scaled <= 2047 ? 'fits' : 'overflow→block++' })` : ''));
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
        const off  = (r % 2) * TABLE_BYTES;
        console.log(`  [${r}] ${RATIOS[r].name.padEnd(12)} ${khz} kHz ± ${tol} kHz  →  page ${page}, off 0x${off.toString(16).toUpperCase().padStart(4, '0')}`);
    }
}

/* ── Генерация бинарного файла ───────────────────────────────────────── *
 * Layout: N таблиц по 8 КБ, упакованных по 2 на 16 КБ страницу.
 *
 *   Page 1: tables[0] (0x0000-0x1FFF) + tables[1] (0x2000-0x3FFF)
 *   Page 2: tables[2] (0x0000-0x1FFF) + tables[3] (0x2000-0x3FFF)
 *   Page 3: tables[4] (0x0000-0x1FFF) + padding
 *
 * Итого: ceil(N/2) × 16 КБ
 */
function generateBinary(outPath) {
    const numPages = Math.ceil(RATIOS.length / 2);
    const totalSize = numPages * PAGE_SIZE;
    const out = Buffer.alloc(totalSize, 0x00);

    for (let r = 0; r < RATIOS.length; r++) {
        const table = generateTable(RATIOS[r].srcPsgHz, HW_CLK_AY);
        const pageIndex = Math.floor(r / 2);
        const slot = r % 2;
        const offset = pageIndex * PAGE_SIZE + slot * TABLE_BYTES;
        table.copy(out, offset);
    }

    fs.writeFileSync(outPath, out);
    console.log(`Written: ${outPath} (${out.length} bytes, ${numPages} pages)`);

    /* Вывод маппинга для кода */
    console.log('\nC mapping (for vgm.c):');
    console.log('// After determining VGM PSG clock, select table:');
    for (let r = 0; r < RATIOS.length; r++) {
        const page = Math.floor(r / 2) + PAGE_BASE;
        const base = (r % 2) * TABLE_BYTES;
        console.log(`//   ${RATIOS[r].srcPsgHz} Hz → wc_mng0_pl(${page}), lut_base = 0x${base.toString(16).padStart(4, '0')}`);
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
 * Алгоритм на Z80 (в vgm.c):
 *   1. bypass: |psg_khz - 1750| ≤ 35 → нет масштабирования
 *   2. Линейный поиск: |psg_khz - entry.clk_khz| ≤ entry.tol_khz → match
 *   3. Нет match → runtime generation (fallback num/den)
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
    h += `/* YM2203: bypass |psg_khz - ${Math.round(HW_CLK_YM2203/1000)}| <= ${Math.round(HW_CLK_YM2203/1000 * BYPASS_PCT / 100)} */\n`;
    h += `#define FREQ_LUT_HW_YM_KHZ     ${Math.round(HW_CLK_YM2203/1000)}u\n`;
    h += `#define FREQ_LUT_BYPASS_TOL_YM  ${Math.round(HW_CLK_YM2203/1000 * BYPASS_PCT / 100)}u  /* ${BYPASS_PCT}% */\n\n`;

    h += `#define FREQ_LUT_AY_COUNT     ${RATIOS.length}u\n`;
    h += `#define FREQ_LUT_YM_COUNT     ${ymEntries.length}u\n`;
    h += `#define FREQ_LUT_ENTRIES      ${TABLE_ENTRIES}u   /* 12-bit PSG */\n`;
    h += `#define FREQ_LUT_FM_MAX       2048u  /* 11-bit FM F-Number */\n`;
    h += `#define FREQ_LUT_BYTES        ${TABLE_BYTES}u     /* per table */\n\n`;

    h += 'typedef struct {\n';
    h += '    uint16_t clk_khz;   /* canonical PSG clock in kHz  */\n';
    h += '    uint16_t tol_khz;   /* ±tolerance in kHz (5%)      */\n';
    h += '    uint8_t  page;      /* plugin page (PAGE_BASE-based) */\n';
    h += '    uint16_t offset;    /* byte offset within page     */\n';
    h += '} freq_lut_entry_t;\n\n';

    h += `/* AY/YM2149 table map (tolerance = ${MATCH_TOLERANCE_PCT}% of canonical PSG clock) */\n`;
    h += 'static const freq_lut_entry_t freq_lut_map_ay[FREQ_LUT_AY_COUNT] = {\n';

    for (let r = 0; r < RATIOS.length; r++) {
        const khz = Math.round(RATIOS[r].srcPsgHz / 1000);
        const tol = Math.round(khz * MATCH_TOLERANCE_PCT / 100);
        const page = Math.floor(r / 2) + PAGE_BASE;
        const off  = (r % 2) * TABLE_BYTES;
        const comma = (r < RATIOS.length - 1) ? ',' : ' ';
        h += `    { ${khz.toString().padStart(4)}u, ${tol.toString().padStart(3)}u, ${page}, 0x${off.toString(16).toUpperCase().padStart(4, '0')} }${comma}  /* ${RATIOS[r].name} */\n`;
    }
    h += '};\n\n';

    /* YM2203 map: full master clock → same table pages (ratio identical) */
    h += `/* YM2203 table map (full master clock, same tables as AY) */\n`;
    h += 'static const freq_lut_entry_t freq_lut_map_ym[FREQ_LUT_YM_COUNT] = {\n';

    let ymIdx = 0;
    for (let r = 0; r < RATIOS.length; r++) {
        if (!RATIOS[r].srcYM2203) continue;
        const khz = Math.round(RATIOS[r].srcYM2203 / 1000);
        const tol = Math.round(khz * MATCH_TOLERANCE_PCT / 100);
        const page = Math.floor(r / 2) + PAGE_BASE;
        const off  = (r % 2) * TABLE_BYTES;
        ymIdx++;
        const comma = (ymIdx < ymEntries.length) ? ',' : ' ';
        h += `    { ${khz.toString().padStart(4)}u, ${tol.toString().padStart(3)}u, ${page}, 0x${off.toString(16).toUpperCase().padStart(4, '0')} }${comma}  /* ${RATIOS[r].name} (YM2203@${khz}kHz) */\n`;
    }
    h += '};\n\n';

    h += '/*\n';
    h += ' * Usage in vgm.c (parse_header):\n';
    h += ' *\n';
    h += ' *   uint16_t psg_khz = chip_clock / 1000;  // already have clock_khz\n';
    h += ' *\n';
    h += ' *   // 1. Bypass check\n';
    h += ' *   uint16_t diff = (psg_khz > FREQ_LUT_HW_KHZ)\n';
    h += ' *                  ? psg_khz - FREQ_LUT_HW_KHZ\n';
    h += ' *                  : FREQ_LUT_HW_KHZ - psg_khz;\n';
    h += ' *   if (diff <= FREQ_LUT_BYPASS_TOL) → no scaling needed\n';
    h += ' *\n';
    h += ' *   // 2. Find matching table (linear scan, 5 entries)\n';
    h += ' *   for (i = 0; i < FREQ_LUT_COUNT; i++) {\n';
    h += ' *       diff = abs16(psg_khz - freq_lut_map[i].clk_khz);\n';
    h += ' *       if (diff <= freq_lut_map[i].tol_khz) {\n';
    h += ' *           wc_mng0_pl(freq_lut_map[i].page);\n';
    h += ' *           lut_base = (uint16_t *)freq_lut_map[i].offset;\n';
    h += ' *           break;\n';
    h += ' *       }\n';
    h += ' *   }\n';
    h += ' *\n';
    h += ' *   // 3. No match → runtime generate into page 1\n';
    h += ' *\n';
    h += ' *   // Access: scaled_period = lut_base[original_period]\n';
    h += ' *   //   PSG: full 12-bit index (0..4095)\n';
    h += ' *   //   FM:  11-bit F-Number index (0..2047)\n';
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

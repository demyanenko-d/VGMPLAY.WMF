/**
 * ihx2wmf.js — Конвертер SDCC .ihx (Intel HEX) + патч заголовка WC → .wmf
 *
 * Использование:
 *   node scripts/ihx2wmf.js <input.ihx> <output.wmf> [--extra <data.bin>]
 *
 * Алгоритм:
 *   1. Парсить Intel HEX, получить бинарные данные (относительные адреса).
 *   2. Определить реальный размер кода (до последнего не-нулевого байта).
 *   3. Вычислить PAGES = ceil(code_size / 16384).
 *   4. Вычислить BLKSIZE = ceil(code_size / 512).
 *   5. Подготовить заголовок плагина (512 байт) с правильными PAGES/BLKSIZE.
 *   6. Если задан --extra: добавить доп. страницы данных (LUT таблицы и т.п.).
 *   7. Записать: [header 512 байт][код][extra data pages].
 *
 * Формат WMF-заголовка (Wild Commander Plugin):
 *   +0   16 байт: зарезервировано
 *   +16  16 байт: "WildCommanderMDL"
 *   +32   1 байт: версия системы плагинов (#10)
 *   +33   1 байт: зарезервировано
 *   +34   1 байт: число 16KB страниц (PAGES)
 *   +35   1 байт: начальная страница (0)
 *   +36  12 байт: 6 × 2 байта блоков данных (blockN: [page, blkcount])
 *   +48  15 байт: зарезервировано
 *   +63   1 байт: флаги (0 = активировать по Enter и F3)
 *   +64  96 байт: расширения (32 × 3 байта, "VGM" + нули)
 *   +160  1 байт: маркер конца расширений (0)
 *   +161  4 байта: макс. размер файла (0xFFFFFFFF = без ограничений)
 *   +165 32 байта: имя плагина (дополнить пробелами)
 *   +197  1 байт: тип активации (0 = по расширению)
 *   +198  6 байт: текст кнопок F2/F4
 *   +204 24 байта: текст меню
 *   +228 32 байта: зарезервировано
 *   +260  1 байт: флаги INI
 *   +261 ... паддинг до 512 байт
 */

'use strict';

const fs   = require('fs');
const path = require('path');

/* ── Константы заголовка ─────────────────────────────────────────────── */
const HEADER_SIZE  = 512;
const PLUGIN_ORG   = 0x8000;   /* виртуальный адрес загрузки WC */
const PLUGIN_NAME  = 'VGM Player OPL3 v2.0   SDCC   ';  /* 32 байта */

/* ── Парсер Intel HEX ────────────────────────────────────────────────── */
function parseIHX(src) {
    const buf  = Buffer.alloc(0x10000, 0x00);
    let   minA = 0x10000;
    let   maxA = 0;

    for (const rawLine of src.split('\n')) {
        const line = rawLine.trim();
        if (!line.startsWith(':')) continue;

        const byteCount  = parseInt(line.slice(1, 3),  16);
        const address    = parseInt(line.slice(3, 7),  16);
        const recordType = parseInt(line.slice(7, 9),  16);

        if (recordType === 0x00) {  /* Data */
            for (let i = 0; i < byteCount; i++) {
                const b = parseInt(line.slice(9 + i * 2, 11 + i * 2), 16);
                const a = address + i;
                buf[a] = b;
                if (a < minA) minA = a;
                if (a > maxA) maxA = a;
            }
        }
        /* recordType 01 = EOF, игнорируем остальное */
    }

    return { buf, minAddr: minA, maxAddr: maxA };
}

/* ── Построить заголовок WC ──────────────────────────────────────────── */
/**
 * @param {number} totalPages — общее число 16KB страниц
 * @param {Array<{page: number, blocks: number}>} blockEntries — блоки
 */
function buildHeader(totalPages, blockEntries) {
    const hdr = Buffer.alloc(HEADER_SIZE, 0x00);

    /* +16: сигнатура */
    Buffer.from('WildCommanderMDL').copy(hdr, 16);

    /* +32: версия системы плагинов */
    hdr[32] = 0x10;

    /* +34: число страниц */
    hdr[34] = totalPages;
    /* +35: начальная страница */
    hdr[35] = 0;

    /* +36: block table — до 6 записей × 2 байта */
    for (let i = 0; i < Math.min(blockEntries.length, 6); i++) {
        hdr[36 + i * 2]     = blockEntries[i].page;
        hdr[36 + i * 2 + 1] = blockEntries[i].blocks;
    }

    /* +63: флаги */
    hdr[63] = 0;

    /* +64: расширения: "VGM", "VGZ" */
    hdr[64] = 0x56; hdr[65] = 0x47; hdr[66] = 0x4D;  /* V G M */
    hdr[67] = 0x56; hdr[68] = 0x47; hdr[69] = 0x5A;  /* V G Z */
    /* остальные 30 × 3 = 90 нулей уже есть */

    /* +160: маркер конца */
    hdr[160] = 0;

    /* +161: макс. размер файла = 0xFFFFFFFF */
    hdr[161] = 0xFF; hdr[162] = 0xFF; hdr[163] = 0xFF; hdr[164] = 0xFF;

    /* +165: имя плагина (32 байта, пробелами) */
    const name = PLUGIN_NAME.slice(0, 32).padEnd(32, ' ');
    Buffer.from(name, 'ascii').copy(hdr, 165);

    /* +197: тип активации */
    hdr[197] = 0;

    return hdr;
}

/* ── Основная логика ─────────────────────────────────────────────────── */
function main() {
    const args = process.argv.slice(2);
    let inFile = null, outFile = null, extraFile = null;

    for (let i = 0; i < args.length; i++) {
        if (args[i] === '--extra' && args[i + 1]) {
            extraFile = args[++i];
        } else if (!inFile) {
            inFile = args[i];
        } else if (!outFile) {
            outFile = args[i];
        }
    }

    if (!inFile || !outFile) {
        console.error('Usage: node ihx2wmf.js <input.ihx> <output.wmf> [--extra <data.bin>]');
        process.exit(1);
    }

    const src    = fs.readFileSync(inFile, 'utf8');
    const { buf, minAddr, maxAddr } = parseIHX(src);

    if (minAddr > maxAddr) {
        console.error('ERROR: empty IHX');
        process.exit(1);
    }

    /* Код начинается с PLUGIN_ORG = 0x8000 */
    if (minAddr < PLUGIN_ORG) {
        console.error(`ERROR: code starts at 0x${minAddr.toString(16)}, expected >= 0x${PLUGIN_ORG.toString(16)}`);
        process.exit(1);
    }

    const codeSize = maxAddr - PLUGIN_ORG + 1;
    const codePages = Math.max(1, Math.ceil(codeSize / 16384));
    const codeBlocks = Math.max(1, Math.ceil(codeSize / 512));

    console.log(`Code: 0x${PLUGIN_ORG.toString(16)} - 0x${maxAddr.toString(16)} (${codeSize} bytes)`);
    console.log(`Code pages: ${codePages}, Code blocks: ${codeBlocks}`);

    /* Блоки: каждая 16KB-страница = один block entry (max 32 секторов).
     *
     * WC читает WMF последовательно и загружает каждый block в свою страницу.
     * Максимум 32 сектора на запись (= 16384 байт = 1 страница).
     * Если код занимает >1 страницы — разбиваем на отдельные block entry.
     *
     * При наличии extra: код паддится до границы 16KB перед extra,
     * поэтому последняя страница кода всегда = 32 сектора (кроме случая
     * когда это единственная страница и extra нет).
     */
    let extraBuf = null;

    if (extraFile) {
        extraBuf = fs.readFileSync(extraFile);
    }

    /* ── Block entries для кода: по одной на страницу ────────────── */
    const blockEntries = [];
    for (let p = 0; p < codePages; p++) {
        if (p < codePages - 1) {
            /* Промежуточные страницы всегда полные */
            blockEntries.push({ page: p, blocks: 32 });
        } else {
            /* Последняя (или единственная) страница кода */
            if (extraBuf) {
                /* С extra: код паддится до 16KB → полная страница */
                blockEntries.push({ page: p, blocks: 32 });
            } else {
                /* Без extra: реальный размер */
                const pageStart = p * 16384;
                const remaining = codeSize - pageStart;
                blockEntries.push({ page: p, blocks: Math.ceil(remaining / 512) });
            }
        }
    }

    /* ── Block entries для extra data: по одной на страницу ──────── */
    if (extraBuf) {
        const extraPages = Math.ceil(extraBuf.length / 16384);
        console.log(`Extra: ${extraFile} (${extraBuf.length} bytes, ${extraPages} pages)`);

        for (let p = 0; p < extraPages; p++) {
            const pageStart = p * 16384;
            const remaining = extraBuf.length - pageStart;
            const pageSize  = Math.min(remaining, 16384);
            const blk       = Math.ceil(pageSize / 512);
            blockEntries.push({ page: codePages + p, blocks: blk });
        }
    }

    const totalPages = blockEntries.reduce((sum, e) => Math.max(sum, e.page + 1), 0);

    if (blockEntries.length > 6) {
        console.error(`ERROR: too many block entries (${blockEntries.length}, max 6)`);
        process.exit(1);
    }

    const hdr       = buildHeader(totalPages, blockEntries);
    const codeSlice = buf.slice(PLUGIN_ORG, PLUGIN_ORG + codeSize);

    /* Итоговый файл: [512 header][код][padding до 16KB boundary][extra pages] */
    const parts = [hdr, codeSlice];

    if (extraBuf) {
        /* Паддинг кода до границы 16KB-страницы
         * (WC загружает каждый блок в свою страницу) */
        const codePadded = codePages * 16384;
        const pad = codePadded - codeSize;
        if (pad > 0) {
            parts.push(Buffer.alloc(pad, 0x00));
        }
        parts.push(extraBuf);
        /* Паддинг extra до конца последней страницы */
        const extraPadded = Math.ceil(extraBuf.length / 16384) * 16384;
        const extraPad = extraPadded - extraBuf.length;
        if (extraPad > 0) {
            parts.push(Buffer.alloc(extraPad, 0x00));
        }
    }

    const out = Buffer.concat(parts);
    fs.writeFileSync(outFile, out);

    console.log(`Pages: ${totalPages}, Block entries: ${blockEntries.length}`);
    for (let i = 0; i < blockEntries.length; i++) {
        console.log(`  block[${i}]: page=${blockEntries[i].page}, sectors=${blockEntries[i].blocks}`);
    }
    console.log(`Written: ${outFile} (${out.length} bytes)`);
}

main();

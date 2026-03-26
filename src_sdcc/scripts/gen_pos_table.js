/**
 * @file    gen_pos_table.js
 * @brief   Генератор таблицы позиций INT для ISR
 *
 * Параметры:
 *   entries  = 56      (прерываний на кадр)
 *   step     = 1280    (T-states между INT)
 *   line_len = 224     (T-states в строке = 448 пикс / 2)
 *
 * Частота ISR: 3,500,000 / 1280 = 2734.375 Гц (~44100/16)
 * Замыкание: 56 x 1280 = 71680 T = 320 строк x 224 T (1 TV-кадр)
 *
 * Формат записи (5 байт):
 *   [VSINTH, VSINTL, HSINT, next_lo, next_hi]
 *
 * OUTI-цикл в ISR: BC начинается с #24AF:
 *   OUTI #24AF -> VSINTH (бит0 = VCNT[8])
 *   OUTI #23AF -> VSINTL (VCNT[7:0])
 *   OUTI #22AF -> HSINT  (HCNT[8:1], 0–223)
 *
 * Использование:
 *   node scripts/gen_pos_table.js
 *   Вывод: asm/pos_table.s
 */

'use strict';

const path = require('path');
const fs   = require('fs');

/* CLI: --entries N --step N */
let entries = 56, step = 1280;
for (let i = 2; i < process.argv.length; i++) {
    if (process.argv[i] === '--entries' && process.argv[i+1]) entries = parseInt(process.argv[++i], 10);
    else if (process.argv[i] === '--step' && process.argv[i+1]) step = parseInt(process.argv[++i], 10);
}

const ENTRIES  = entries;
const STEP     = step;
const LINE_LEN = 224;   /* T-states per line at 3.5 MHz */
const ISR_FREQ = Math.round(3_500_000 / STEP);

const BASE_ADDR_STR = 'pos_table'; /* метка в ASM */

/* Вычислить символьный адрес записи по индексу.
 * В sdas каждая запись занимает 5 байт: 3 байта DB + 2 байта DW.
 * sdas не умеет $+N в секции _CODE напрямую, поэтому используем
 * явные метки _pt_0, _pt_1, ... для ссылок next_ptr. */

const lines = [
    ';============================================================================',
    '; pos_table.s — Таблица позиций INT (СГЕНЕРИРОВАНО, не редактировать)',
    ';',
    `; entries=${ENTRIES}, step=${STEP}, line_len=${LINE_LEN}`,
    `; ISR freq: 3,500,000 / ${STEP} = ${ISR_FREQ} Hz (~44100/16)`,
    ';============================================================================',
    '',
    '        .module pos_table_mod',
    '        .globl  pos_table',
    '',
    '        .area _CODE',
    '',
    'pos_table:',
];

for (let i = 0; i < ENTRIES; i++) {
    const pos   = i * STEP;
    const line  = Math.floor(pos / LINE_LEN);
    const hpos  = pos % LINE_LEN;

    const vsinth = (line >> 8) & 0x01;   /* только бит 0 (VCNT[8]) */
    const vsintl = line & 0xFF;
    const hsint  = hpos & 0xFF;

    const nextLabel = (i < ENTRIES - 1)
        ? `_pt_${i + 1}`
        : `pos_table`;

    lines.push(`_pt_${i}:`);
    lines.push(
        `        .db   0x${vsinth.toString(16).padStart(2,'0').toUpperCase()}` +
        `, 0x${vsintl.toString(16).padStart(2,'0').toUpperCase()}` +
        `, 0x${hsint .toString(16).padStart(2,'0').toUpperCase()}` +
        `        ; [${String(i).padStart(2)}] line=${String(line).padStart(3)}, hpos=${String(hpos).padStart(3)}`
    );
    lines.push(`        .dw   ${nextLabel}`);
}

lines.push('');
lines.push('        .globl pos_table_end');
lines.push('pos_table_end:');
lines.push('');

const outPath = path.join(__dirname, '..', 'asm', 'pos_table.s');
fs.writeFileSync(outPath, lines.join('\n'), 'utf8');

console.log(`OK: ${ENTRIES} entries, ${ENTRIES * 5} bytes → ${outPath}`);

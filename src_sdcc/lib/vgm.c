/**
 * @file    vgm.c
 * @brief   VGM-парсер для ISR-буферов (OPL/AY/SAA + dual-chip)
 * @version 4.0
 *
 * Читает VGM-поток из VPL-страниц WC (окно #C000).
 *
 * Поддержка чипов (воспроизведение):
 *   YM3812 (OPL2):  cmd 0x5A → CMD_WRITE_B0   (clock @ header 0x50)
 *   YM3526 (OPL1):  cmd 0x5B → CMD_WRITE_B0   (clock @ header 0x54)
 *   Y8950  (OPL):   cmd 0x5C → CMD_WRITE_B0   (clock @ header 0x58)
 *   YMF262 (OPL3):  cmd 0x5E → CMD_WRITE_B0   (clock @ header 0x5C)
 *                    cmd 0x5F → CMD_WRITE_B1
 *   AY8910 (PSG):   cmd 0xA0 → CMD_WRITE_AY / CMD_WRITE_AY2 (dual)
 *   YM2203 (OPN):   cmd 0x55 → CMD_WRITE_AY (PSG-часть)
 *   SAA1099:        cmd 0xBD → CMD_WRITE_SAA (bit7 = chip select)
 *
 * Распознавание (parse_header, vgm_chip_list[]):
 *   Все 40 чипов VGM spec, включая dual-chip (бит 30 clock).
 *
 * Масштабирование: 44100 / ISR_FREQ ≈ 2^VGM_SAMPLE_SHIFT.
 *
 * Оптимизации:
 *   - vgm_read_byte() — чистый ASM, экономит ~20 байт и ~30T на вызов.
 *   - vgm_skip() — арифметический пропуск без чтения (data block).
 *   - chip_scan[] — компактная ROM-таблица (4 bytes × 40 entries).
 */

#include "../inc/types.h"
#include "../inc/vgm.h"
#include "../inc/isr.h"
#include "../inc/wc_api.h"
#include "../inc/freq_lut_map.h"

/* ── Состояние парсера ──────────────────────────────────────────────── */
volatile uint8_t vgm_song_ended; // 1 = достигнут конец (0x66), main loop может остановить воспроизведение
uint8_t vgm_paused;              // 1 = пауза (остановить таймер, не обнулять vgm_sec_budget)
uint8_t vgm_cur_page;            // текущая VPL-страница для чтения данных (0..N)
uint16_t vgm_read_ptr;           // смещение в окне #C000 для чтения следующего байта VGM-потока
uint8_t vgm_end_page;            // страница, на которой заканчивается VGM-поток (для оптимизации проверки конца)
uint16_t vgm_end_addr;           // смещение в окне #C000, на котором заканчивается VGM-поток (для оптимизации проверки конца)
uint16_t vgm_version;            // версия VGM в BCD (например, 0x0151 для v1.51)
uint8_t vgm_chip_type;           // битовая маска типа чипов (VGM_CHIP_xxx), например VGM_CHIP_OPL2 | VGM_CHIP_AY
uint8_t vgm_chip_count;          // количество обнаруженных чипов (для вывода информации о файле)

vgm_chip_entry_t vgm_chip_list[VGM_MAX_CHIPS];

uint16_t vgm_loop_addr;          // адрес точки петли в окне #C000 (0 = нет петли)
uint8_t vgm_loop_page;           // страница, на которой находится точка петли
uint16_t vgm_total_seconds;      // общее время воспроизведения в секундах
uint8_t  vgm_loop_enabled;       // 1 = петля включена, 0 = нет

// параметры политики петли (читаются из INI, устанавливаются main.c)
uint8_t  cfg_loop_rewinds  = 1;
uint8_t  cfg_min_duration  = 10;
uint16_t cfg_max_duration  = 240;

// очередь высокоуровневых команд для vgm_fill_buffer (play, loop, init, silence)
hl_entry_t vgm_hl_queue[HL_QUEUE_MAX];
uint8_t vgm_hl_len;
uint8_t vgm_hl_pos;
uint8_t vgm_hl_abort_pos;
uint8_t vgm_loop_count;


/* ── Аккумулятор остатка задержки (VGM samples, 0..15) ─────────────── */
static uint16_t vgm_wait_accum = 0;

/* ── Битовая маска spectrum analyzer (fill_buffer → pad bytes → ISR) ── */
/* Не static: используется ASM-хелперами spectrum (asm/spectrum.s)      */
uint16_t spec_mask;            /* 16-бит: bit N → полоса N на максимум   */
uint16_t spec_ay_period[6];    /* AY period shadow: ch0-2(chip1), ch0-2(chip2) */
uint8_t  spec_saa_oct[6];      /* SAA octave shadow: chip 0 каналы 0-5  */
uint8_t  spec_saa_oct2[6];     /* SAA octave shadow: chip 1 каналы 0-5  */
uint8_t  spec_saa_dual;        /* 1=dual SAA: bars 0-7 / 8-15 split     */
uint8_t  cfg_saa_mode = 0;     /* 0=chip0 only(default), 1=chip1 on chip0, 2=turbo(both) */
uint8_t  spec_opl_bd;          /* OPL 0xBD shadow для детекции фронта    */
uint8_t  spec_fm_block[6];     /* YM2203 FM block shadow: ch0-2(chip1/2)*/

/* ── Масштабирование частот PSG/FM (LUT + asm) ──────────────────────── */
uint8_t vgm_freq_mode;
uint16_t *freq_lut_base;
uint16_t vgm_freq_lut_khz;      /* clock (кГц) подобранной LUT таблицы  */
static uint8_t freq_lut_page;   /* plugin page для Window 0 (LUT)       */

/* FM F-Number: множитель N для формулы fnum × N / 7.
 * 0 = bypass (масштабирование не нужно), иначе 3/5/6/8. */
static uint8_t fm_mul;

/* Shadow-регистры PSG: полный 12-бит tone period на канал.
 * Обновляются при записи lo/hi, используются для LUT lookup. */
static uint16_t psg_shadow[3];   /* каналы 0,1,2 — chip 1 */
static uint16_t psg_shadow2[3];  /* каналы 0,1,2 — chip 2 (0xA5) */

/* Shadow-регистры FM: 11-бит F-Number + 3-бит block на канал.
 * Обновляются при записи 0xA0-0xA2 (lo) / 0xA4-0xA6 (hi+block). */
static uint16_t fm_fnum[3];     /* F-Number каналы 0,1,2 — chip 1 */
static uint16_t fm_fnum2[3];    /* F-Number каналы 0,1,2 — chip 2 */
static uint8_t  fm_block[3];    /* block каналы 0,1,2 — chip 1    */
static uint8_t  fm_block2[3];   /* block каналы 0,1,2 — chip 2    */

/* ── Бюджеты для вставки служебных ISR-команд ──────────────────────── */
static uint16_t vgm_sec_budget = ISR_FREQ;  /* тиков до CMD_INC_SEC  */

/* ── Статические локальные для vgm_fill_buffer / emit_wait ──────────
 * Устраняет IX-frame SDCC (32 байта стека + 19T на ld r,-N(ix)).
 * Static доступ: ld a,(nn) = 13T, ld hl,(nn) = 16T.                   */
static uint8_t *fb_buf;    /* база текущего cmd_buf                     */
static uint8_t *fb_wp;     /* указатель записи в cmd_buf                */
static uint8_t *fb_rp;     /* указатель чтения VGM (#C000-#FFFF)       */
static uint8_t  fb_cpg;    /* локальная копия vgm_cur_page              */
static uint8_t  fb_budget; /* команд до принудительного yield            */
static uint8_t  fb_yield_ticks; /* тиков потрачено на budget yield-ы     */
static uint16_t fb_wacc;   /* аккумулятор задержки (0..VGM_SAMPLE_MASK)  */
static uint8_t  fb_op;     /* текущий VGM opcode                       */
static uint8_t  fb_b1;     /* байт операнда 1                           */
static uint8_t  fb_b2;     /* байт операнда 2                           */
static uint8_t  fb_is_vgm; /* 1 = VGM mega-buffer, 0 = cmdblk          */
static uint16_t fb_tk;     /* temp: счётчик тиков для emit_wait         */
static uint32_t fb_t32;    /* temp: 32-бит размер data-block (0x67)     */
static uint16_t fb_w;      /* temp: промежуточное значение emit_wait    */
static uint8_t  fb_n;      /* temp: число байт для пропуска             */
static uint8_t *fb_end;    /* предвычисленный fb_buf + (CMD_BUF_SIZE-48) */
static hl_entry_t *fb_e;   /* temp: указатель на запись HL queue         */
static uint8_t  fb_ch;     /* temp: индекс канала PSG/FM                */
static uint16_t fb_scaled; /* temp: масштабированная частота             */
static uint8_t  fb_reg;    /* temp: AY регистр (обработчик 0xA0)        */

/* VGM_FILL_CMD_BUDGET определён в variant_cfg.h (через isr.h) */

/* ── Хелперы spectrum analyzer (ASM: asm/spectrum.s) ────────────── */
/* 16 полос = 8 октав × 2 поддиапазона (по OPL block*2+fnum_msb).
 * Все реализации на оптимизированном Z80 ASM для горячего цикла.    */
extern void spectrum_opl_b0(uint8_t reg, uint8_t val);
extern void spectrum_opl_b1(uint8_t reg, uint8_t val);
extern void spectrum_ay(uint8_t reg, uint8_t val);
extern void spectrum_ay2(uint8_t reg, uint8_t val);
extern void spectrum_saa(uint8_t reg, uint8_t val);
extern void spectrum_saa2(uint8_t reg, uint8_t val);


/* ── Таблица сканирования чипов (ROM-const) ──────────────────────── */
/* Каждая запись: { offset_in_header, min_version_lo }
 * offset — байтовое смещение 32-bit clock-поля в VGM заголовке.
 * minv_lo — младший байт минимальной VGM-версии (старший всегда 0x01).
 * chip ID в chip_list = offset. */
typedef struct
{
    uint8_t off;
    uint8_t minv_lo;
} chip_scan_t;

static const chip_scan_t chip_scan[] = {
    {0x0C, 0x00}, {0x10, 0x00},                             /* 1.00 */
    {0x2C, 0x50}, {0x30, 0x50},                             /* 1.50 */
    {0x38, 0x51}, {0x40, 0x51}, {0x44, 0x51}, {0x48, 0x51}, /* 1.51 */
    {0x4C, 0x51}, {0x50, 0x51}, {0x54, 0x51}, {0x58, 0x51},
    {0x5C, 0x51}, {0x60, 0x51}, {0x64, 0x51}, {0x68, 0x51},
    {0x6C, 0x51}, {0x70, 0x51}, {0x74, 0x51},
    {0x80, 0x61}, {0x84, 0x61}, {0x88, 0x61}, {0x8C, 0x61}, /* 1.61 */
    {0x90, 0x61}, {0x98, 0x61}, {0x9C, 0x61}, {0xA0, 0x61},
    {0xA4, 0x61}, {0xA8, 0x61}, {0xAC, 0x61}, {0xB0, 0x61},
    {0xB4, 0x61},
    {0xB8, 0x71}, {0xC0, 0x71}, {0xC4, 0x71}, {0xC8, 0x71}, /* 1.71 */
    {0xCC, 0x71}, {0xD0, 0x71}, {0xD8, 0x71}, {0xDC, 0x71},
    {0xE0, 0x71},
};
#define CHIP_SCAN_COUNT (sizeof(chip_scan)/sizeof(chip_scan[0]))

/* ── Локальный хелпер ───────────────────────────────────────────────── */

/* Пропустить n байт в VGM-потоке (арифметически, без чтения).
 * Используется для data block 0x67, PCM RAM write 0x68 и др. */
static void vgm_skip(uint32_t n)
{
    uint16_t ptr = vgm_read_ptr;
    uint32_t avail = 0x10000UL - (uint32_t)ptr;

    if (n < avail) {
        vgm_read_ptr = ptr + (uint16_t)n;
        return;
    }

    n -= avail;
    vgm_cur_page++;

    while (n >= 0x4000UL) {
        n -= 0x4000UL;
        vgm_cur_page++;
    }

    vgm_read_ptr = 0xC000 + (uint16_t)n;
    wc_mngcvpl(vgm_cur_page);
}

/* ─────────────────────────────────────────────────────────────────────
 * vgm_parse_header
 * ───────────────────────────────────────────────────────────────────── */
uint8_t vgm_parse_header(void)
{
    uint8_t  *base = (uint8_t *)0xC000;
    uint32_t  data_off_field;
    uint32_t  data_start_abs;
    uint32_t  loop_off_field;
    uint32_t  loop_start_abs;
    uint32_t  eof_off_field;
    uint32_t  end_file_off;
    uint8_t   i;
    uint8_t   off_hdr;
    uint32_t  clk;
    vgm_chip_entry_t *e;

    vgm_song_ended = 0;
    vgm_paused = 0;
    vgm_chip_count = 0;
    vgm_chip_type = VGM_CHIP_NONE;
    vgm_loop_addr = 0;
    vgm_loop_page = 0;
    vgm_wait_accum = 0;
    vgm_sec_budget = ISR_FREQ;

    /* Сигнатура "Vgm " */
    if (base[0] != 'V' || base[1] != 'g' || base[2] != 'm' || base[3] != ' ')
        return VGM_ERR_HEADER;

    /* Версия */
    vgm_version = (uint16_t)base[0x08] | ((uint16_t)base[0x09] << 8);

    /* ── Data offset ──────────────────────────────────────────────────
     * VGM >= 1.50: поле в 0x34 — относительное смещение от 0x34.
     * Если поле == 0, для совместимости используем 0x40.
     * VGM < 1.50: data начинается с 0x40.
     */
    if (vgm_version >= 0x0150) {
        data_off_field = (uint32_t)base[0x34]
                       | ((uint32_t)base[0x35] << 8)
                       | ((uint32_t)base[0x36] << 16)
                       | ((uint32_t)base[0x37] << 24);

        data_start_abs = data_off_field ? (0x34UL + data_off_field) : 0x40UL;
    } else {
        data_start_abs = 0x40UL;
    }

    /* Если data_start выходит за EOF — файл сломан */
    eof_off_field = (uint32_t)base[0x04]
                  | ((uint32_t)base[0x05] << 8)
                  | ((uint32_t)base[0x06] << 16)
                  | ((uint32_t)base[0x07] << 24);
    end_file_off = eof_off_field + 4UL;

    if (data_start_abs >= end_file_off)
        return VGM_ERR_HEADER;

    vgm_cur_page = (uint8_t)(data_start_abs >> 14);
    vgm_read_ptr = 0xC000 + (uint16_t)(data_start_abs & 0x3FFF);

    /* ── Конец файла ────────────────────────────────────────────────── */
    vgm_end_page = (uint8_t)(end_file_off >> 14);
    vgm_end_addr = 0xC000 + (uint16_t)(end_file_off & 0x3FFF);

    /* ── Scan чипов ─────────────────────────────────────────────────── */
    for (i = 0; i < CHIP_SCAN_COUNT; i++) {
        off_hdr = chip_scan[i].off;

        if (vgm_version < (0x0100u | chip_scan[i].minv_lo))
            continue;

        /* Clock fields должны лежать до начала VGM data stream */
        if ((uint32_t)off_hdr + 4UL > data_start_abs)
            continue;

        /* Читаем clock (32-bit LE) */
        clk  = (uint32_t)base[off_hdr];
        clk |= (uint32_t)base[off_hdr + 1] << 8;
        clk |= (uint32_t)base[off_hdr + 2] << 16;
        clk |= (uint32_t)base[off_hdr + 3] << 24;

        if (clk == 0)
            continue;

        /* Сохраняем */
        if (vgm_chip_count < VGM_MAX_CHIPS) {
            e = &vgm_chip_list[vgm_chip_count];
            e->id = off_hdr;
            e->flags = (clk & 0x40000000UL) ? VGM_CF_DUAL : 0;
            clk &= 0x3FFFFFFFUL;
            e->clock_khz = (uint16_t)(clk / 1000UL);
            vgm_chip_count++;
        }

        /* Определяем тип OPL для fill_buffer */
        if (off_hdr == VGM_OFF_YMF262)
            vgm_chip_type = VGM_CHIP_OPL3;
        else if ((off_hdr == VGM_OFF_YM3812 || off_hdr == VGM_OFF_Y8950) && vgm_chip_type < VGM_CHIP_OPL2)
            vgm_chip_type = VGM_CHIP_OPL2;
        else if (off_hdr == VGM_OFF_YM3526 && vgm_chip_type < VGM_CHIP_OPL)
            vgm_chip_type = VGM_CHIP_OPL;
    }

    /* ── Подбор LUT для пересчёта частот PSG/FM ────────────────────────
     * YM2203: psg_khz = полный master clock → ищем в freq_lut_map_ym[]
     * AY8910: psg_khz = clock как есть      → ищем в freq_lut_map_ay[]
     *
     * PSG таблица в 16KB странице (2 per page, PSG_A@0x0000, PSG_B@0x2000).
     * PSG ratio = target/source (period scaling).
     * FM  ratio = source/target (F-Number scaling, ОБРАТНЫЙ!) — ASM ×N/7. */
    vgm_freq_mode = FREQ_MODE_NATIVE;
    freq_lut_base = (uint16_t *)0;
    freq_lut_page = 0;
    vgm_freq_lut_khz = 0;
    fm_mul = 0;
    psg_shadow[0] = 0; psg_shadow[1] = 0; psg_shadow[2] = 0;
    psg_shadow2[0] = 0; psg_shadow2[1] = 0; psg_shadow2[2] = 0;
    fm_fnum[0] = 0; fm_fnum[1] = 0; fm_fnum[2] = 0;
    fm_fnum2[0] = 0; fm_fnum2[1] = 0; fm_fnum2[2] = 0;
    fm_block[0] = 0; fm_block[1] = 0; fm_block[2] = 0;
    fm_block2[0] = 0; fm_block2[1] = 0; fm_block2[2] = 0;

    for (i = 0; i < vgm_chip_count; i++) {
        uint16_t psg_khz;
        uint16_t hw_khz;
        uint16_t bypass_tol;
        const freq_lut_entry_t *lut_map;
        uint8_t lut_count;
        uint8_t is_ym;

        if (vgm_chip_list[i].id == VGM_OFF_YM2203) {
            psg_khz    = vgm_chip_list[i].clock_khz;  /* полный master clock */
            hw_khz     = FREQ_LUT_HW_YM_KHZ;          /* 3500 */
            bypass_tol = FREQ_LUT_BYPASS_TOL_YM;       /* 70   */
            lut_map    = freq_lut_map_ym;
            lut_count  = FREQ_LUT_YM_COUNT;
            is_ym      = 1;
        } else if (vgm_chip_list[i].id == VGM_OFF_AY8910) {
            psg_khz    = vgm_chip_list[i].clock_khz;
            hw_khz     = FREQ_LUT_HW_KHZ;              /* 1750 */
            bypass_tol = FREQ_LUT_BYPASS_TOL;           /* 35   */
            lut_map    = freq_lut_map_ay;
            lut_count  = FREQ_LUT_AY_COUNT;
            is_ym      = 0;
        } else {
            continue;
        }

        /* ── Определяем режим масштабирования ─────────────────────
         * 1) bypass: |psg_khz - HW| ≤ 2% → native
         * 2) LUT table match (±5%) → TABLE mode (Window 0)
         * Нет match → остаётся native (экзотика <1% файлов) */
        {
            uint16_t diff = (psg_khz > hw_khz)
                          ? (psg_khz - hw_khz)
                          : (hw_khz - psg_khz);
            if (diff <= bypass_tol) {
                /* ±2% — native, пересчёт не нужен */
                vgm_freq_mode = FREQ_MODE_NATIVE;
            } else {
                /* Ищем подходящую предрасчитанную таблицу (±5%) */
                uint8_t j;
                for (j = 0; j < lut_count; j++) {
                    uint16_t d = (psg_khz > lut_map[j].clk_khz)
                               ? (psg_khz - lut_map[j].clk_khz)
                               : (lut_map[j].clk_khz - psg_khz);
                    if (d <= lut_map[j].tol_khz) {
                        /* Матч! Подключаем LUT страницу в Window 0 */
                        freq_lut_page = lut_map[j].page;
                        wc_mng0_pl(freq_lut_page);
                        freq_lut_base = (uint16_t *)lut_map[j].offset;
                        /* FM множитель: fnum × N / 7 (в ASM) */
                        if (is_ym) {
                            fm_mul = freq_lut_fm_mul[j];
                        }
                        vgm_freq_lut_khz = lut_map[j].clk_khz;
                        vgm_freq_mode = FREQ_MODE_TABLE;
                        break;
                    }
                }
                /* Нет match → native (без масштабирования) */
            }
        }
        break;
    }

    /* ── Loop offset ──────────────────────────────────────────────────
     * Relative offset from 0x1C. Если значение невалидно — loop отключаем.
     */
    loop_off_field = (uint32_t)base[0x1C]
                   | ((uint32_t)base[0x1D] << 8)
                   | ((uint32_t)base[0x1E] << 16)
                   | ((uint32_t)base[0x1F] << 24);

    if (loop_off_field) {
        loop_start_abs = 0x1CUL + loop_off_field;

        if (loop_start_abs >= data_start_abs && loop_start_abs < end_file_off) {
            vgm_loop_page = (uint8_t)(loop_start_abs >> 14);
            vgm_loop_addr = 0xC000 + (uint16_t)(loop_start_abs & 0x3FFF);
        } else {
            vgm_loop_addr = 0;
            vgm_loop_page = 0;
        }
    }

    /* ── Общая длительность + политика лупа ───────────────────────────
     * total_samples (0x18) = число сэмплов всего файла (до лупа).
     * loop_samples  (0x20) = число сэмплов одной итерации лупа.
     *
     * Loop отключается если:
     *  1) loop_addr == 0 (нет лупа в файле)
     *  2) full-track loop (loop_start == data_start)
     *  3) базовый трек < cfg_min_duration
     *  4) с лупом получается > cfg_max_duration
     */
    {
        uint32_t ts = (uint32_t)base[0x18]
                    | ((uint32_t)base[0x19] << 8)
                    | ((uint32_t)base[0x1A] << 16)
                    | ((uint32_t)base[0x1B] << 24);
        uint16_t base_sec = (uint16_t)(ts / 44100UL);

        vgm_loop_enabled = 0;

        if (vgm_loop_addr &&
            !(vgm_loop_page == vgm_cur_page &&
              vgm_loop_addr == vgm_read_ptr) &&
            base_sec >= cfg_min_duration) {
            /* Частичный loop, трек достаточно длинный — пробуем */
            uint32_t ls = (uint32_t)base[0x20]
                        | ((uint32_t)base[0x21] << 8)
                        | ((uint32_t)base[0x22] << 16)
                        | ((uint32_t)base[0x23] << 24);
            uint32_t total_with_loop = ts + ls * cfg_loop_rewinds;
            uint16_t total_sec = (uint16_t)(total_with_loop / 44100UL);

            if (total_sec <= cfg_max_duration) {
                vgm_loop_enabled = 1;
                base_sec = total_sec;
            }
        }

        vgm_total_seconds = base_sec;
    }

    return VGM_OK;
}

/* ─────────────────────────────────────────────────────────────────────
 * vgm_chip_name — имя чипа по header offset
 * ───────────────────────────────────────────────────────────────────── */
const char *vgm_chip_name(uint8_t id)
{
    switch (id) {
        case 0x0C: return "SN76489";
        case 0x10: return "YM2413";
        case 0x44: return "YM2203";
        case 0x50: return "YM3812";
        case 0x54: return "YM3526";
        case 0x58: return "Y8950";
        case 0x5C: return "YMF262";
        case 0x60: return "YMF278B";
        case 0x74: return "AY8910";
        case 0xC8: return "SAA1099";
        default:   return 0;
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_wait_63 / asm_wait_62 — добавляет 882/735 к fb_wacc, делает
 * >> VGM_SAMPLE_SHIFT, пишет fb_tk и fb_wacc.
 *
 * Заменяет цепочку: C-сложение → fb_t → asm_shift_mask.
 * ~165 T (62) / ~175 T (63)  vs  ~246 T (C + shift_mask).
 *
 * VGM_SAMPLE_SHIFT = 6.  Максимальный результат: (63+882)>>6 = 14.
 * Clobbers: A, B, D, E, H, L.
 * ───────────────────────────────────────────────────────────────────── */
static void asm_wait_62(void) __naked;
static void asm_wait_63(void) __naked;

static void asm_wait_63(void) __naked {
    __asm
    ld   de, #882            ;10T
    jp   _asm_wait_common    ;10T
    __endasm;
}
static void asm_wait_62(void) __naked {
    __asm
    ld   de, #735            ;10T
_asm_wait_common:
    ld   hl, (_fb_wacc)      ;16T
    add  hl, de              ;11T  max 63+882=945=0x03B1, CF=0
    ;; fb_wacc = L & VGM_SAMPLE_MASK
    ld   a, l                ; 4T
    and  a, #VGM_SAMPLE_MASK ; 7T
    ld   (_fb_wacc), a       ;13T
    ;; fb_tk = HL >> VGM_SAMPLE_SHIFT  via  H<<(8-SHIFT) | L>>(SHIFT)
    ld   a, h                ; 4T
    add  a, a                ; 4T  <<1
    add  a, a                ; 4T  <<2
#if VGM_SAMPLE_SHIFT < 6
    add  a, a                ; 4T  <<3
#endif
#if VGM_SAMPLE_SHIFT < 5
    add  a, a                ; 4T  <<4
#endif
    ld   b, a                ; 4T  B = H<<(8-SHIFT)
    ld   a, l                ; 4T
    rlca                     ; 4T
    rlca                     ; 4T
#if VGM_SAMPLE_SHIFT < 6
    rlca                     ; 4T
#endif
#if VGM_SAMPLE_SHIFT < 5
    rlca                     ; 4T
#endif
#if VGM_SAMPLE_SHIFT == 6
    and  a, #0x03            ; 7T  L>>6
#elif VGM_SAMPLE_SHIFT == 5
    and  a, #0x07            ; 7T  L>>5
#elif VGM_SAMPLE_SHIFT == 4
    and  a, #0x0F            ; 7T  L>>4
#endif
    or   a, b                ; 4T  A = fb_tk (max 14, fits u8)
    ld   (_fb_tk), a         ;13T
    xor  a, a                ; 4T
    ld   (_fb_tk+1), a       ;13T
    ld   (_fb_wacc+1), a     ;13T
    ret                      ;10T
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_short_wait — для 0x70-0x7F:  fb_wacc += (fb_op & 0x0F) + 1,
 *                  >> VGM_SAMPLE_SHIFT, пишет fb_tk и fb_wacc.
 *
 * Максимум fb_wacc(63) + 16 = 79 → H=0, fb_tk = 0 или 1.
 * ~175 T total  vs  ~304 T (C + shift_mask).
 * Clobbers: A, D, E, H, L.
 * ───────────────────────────────────────────────────────────────────── */
static void asm_short_wait(void) __naked {
    __asm
    ld   a, (_fb_op)          ;13T
    and  a, #0x0F             ; 7T
    inc  a                    ; 4T  A = (op & 0x0F) + 1  (1..16)
    ld   e, a                 ; 4T
    ld   d, #0                ; 7T  DE = addend
    ld   hl, (_fb_wacc)       ;16T
    add  hl, de               ;11T  max 79, CF=0, H=0
    ;; fb_wacc = L & VGM_SAMPLE_MASK
    ld   a, l                 ; 4T
    and  a, #VGM_SAMPLE_MASK  ; 7T
    ld   (_fb_wacc), a        ;13T
    ;; fb_tk = L >> VGM_SAMPLE_SHIFT  (H guaranteed 0)
    ld   a, l                 ; 4T
    rlca                      ; 4T
    rlca                      ; 4T
#if VGM_SAMPLE_SHIFT < 6
    rlca                      ; 4T
#endif
#if VGM_SAMPLE_SHIFT < 5
    rlca                      ; 4T
#endif
#if VGM_SAMPLE_SHIFT == 6
    and  a, #0x03             ; 7T
#elif VGM_SAMPLE_SHIFT == 5
    and  a, #0x07             ; 7T
#elif VGM_SAMPLE_SHIFT == 4
    and  a, #0x0F             ; 7T
#endif
    ld   (_fb_tk), a          ;13T
    xor  a, a                 ; 4T
    ld   (_fb_tk+1), a        ;13T
    ld   (_fb_wacc+1), a      ;13T
    ret                       ;10T
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_arb_wait — для 0x61:  fb_wacc += fb_b2:fb_b1 (16-бит),
 *                >> VGM_SAMPLE_SHIFT (= 6) с учётом переноса (17-бит).
 * Записывает fb_tk (max 1024), fb_wacc.
 *
 * ~236 T total  vs  SDCC 32-бит IX-frame + DJNZ ~725 T.  (3× быстрее)
 *
 * Вызывать ПОСЛЕ asm_read_2bytes() (fb_b1, fb_b2 уже прочитаны).
 * Clobbers: A, B, C, D, E, H, L.
 * ───────────────────────────────────────────────────────────────────── */
static void asm_arb_wait(void) __naked {
    __asm
    ;; sample_count = fb_b2:fb_b1
    ld   a, (_fb_b2)          ;13T
    ld   h, a                 ; 4T
    ld   a, (_fb_b1)          ;13T
    ld   l, a                 ; 4T
    ;; sum = fb_wacc + sample_count  (17-bit: CF:H:L)
    ld   de, (_fb_wacc)       ;20T
    add  hl, de               ;11T  CF = bit16
    ;; Save carry to B  (ld does not touch flags!)
    ld   b, #0                ; 7T
    jr   nc, 80$              ;12/7T  usually no carry
#if VGM_SAMPLE_SHIFT == 6
    ld   b, #4                ; 7T   CF << (8-6) = CF<<2 = 4
#elif VGM_SAMPLE_SHIFT == 5
    ld   b, #8                ; 7T   CF << (8-5) = CF<<3 = 8
#elif VGM_SAMPLE_SHIFT == 4
    ld   b, #16               ; 7T   CF << (8-4) = CF<<4 = 16
#endif
80$:
    ;; fb_wacc = L & VGM_SAMPLE_MASK
    ld   a, l                 ; 4T
    and  a, #VGM_SAMPLE_MASK  ; 7T
    ld   (_fb_wacc), a        ;13T
    xor  a, a                 ; 4T
    ld   (_fb_wacc+1), a      ;13T
    ;; fb_tk = (CF:H:L) >> VGM_SAMPLE_SHIFT
    ;; low byte: (H<<(8-SHIFT)) | (L>>SHIFT)
    ld   a, l                 ; 4T
    rlca                      ; 4T
    rlca                      ; 4T
#if VGM_SAMPLE_SHIFT < 6
    rlca                      ; 4T
#endif
#if VGM_SAMPLE_SHIFT < 5
    rlca                      ; 4T
#endif
#if VGM_SAMPLE_SHIFT == 6
    and  a, #0x03             ; 7T  L>>6
#elif VGM_SAMPLE_SHIFT == 5
    and  a, #0x07             ; 7T  L>>5
#elif VGM_SAMPLE_SHIFT == 4
    and  a, #0x0F             ; 7T  L>>4
#endif
    ld   c, a                 ; 4T
    ld   a, h                 ; 4T
    rlca                      ; 4T
    rlca                      ; 4T
#if VGM_SAMPLE_SHIFT < 6
    rlca                      ; 4T
#endif
#if VGM_SAMPLE_SHIFT < 5
    rlca                      ; 4T
#endif
    ld   d, a                 ; 4T  D = rotated H
#if VGM_SAMPLE_SHIFT == 6
    and  a, #0xFC             ; 7T  H<<2
#elif VGM_SAMPLE_SHIFT == 5
    and  a, #0xF8             ; 7T  H<<3
#elif VGM_SAMPLE_SHIFT == 4
    and  a, #0xF0             ; 7T  H<<4
#endif
    or   a, c                 ; 4T  lo = (H<<(8-SHIFT))|(L>>SHIFT)
    ld   l, a                 ; 4T
    ;; high byte: (CF<<(8-SHIFT)) | (H>>SHIFT)
    ld   a, d                 ; 4T  rotated H
#if VGM_SAMPLE_SHIFT == 6
    and  a, #0x03             ; 7T  H>>6
#elif VGM_SAMPLE_SHIFT == 5
    and  a, #0x07             ; 7T  H>>5
#elif VGM_SAMPLE_SHIFT == 4
    and  a, #0x0F             ; 7T  H>>4
#endif
    or   a, b                 ; 4T  + CF<<(8-SHIFT)
    ld   h, a                 ; 4T
    ld   (_fb_tk), hl         ;16T
    ret                       ;10T
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_read_byte — чтение одного байта из fb_rp, обработка page-cross
 * Возвращает A = byte.  Clobbers C, HL.
 * Fast-path: 74 T  (+ call 17 + ret 10 = 101 T итого)
 * ───────────────────────────────────────────────────────────────────── */
static uint8_t asm_read_byte(void) __naked
{
    __asm
    ld   hl, (_fb_rp)       ;16  load read pointer
    ld   c, (hl)            ; 7  byte -> C
    inc  hl                 ; 6
    ld   a, h               ; 4
    or   a, l               ; 4  Z if HL wrapped to 0x0000
    jr   Z, 150$            ; 7  page-cross (rare)
    ld   (_fb_rp), hl       ;16  store updated pointer
    ld   a, c               ; 4  result -> A
    ret                     ;10  fast-path 74 T
150$:
    ld   hl, #_fb_cpg       ;10
    inc  (hl)               ;11
    ld   a, (hl)            ; 7
    push bc                 ;11  save result
    call _wc_mngcvpl        ;17+ switch VPL page (A = fb_cpg)
    pop  bc                 ;10
    ld   hl, #0xC000        ;10
    ld   (_fb_rp), hl       ;16
    ld   a, c               ; 4  result -> A
    ret                     ;10
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_read_2bytes — чтение двух последовательных байт → fb_b1, fb_b2
 * Пишет напрямую в static переменные.  Обрабатывает page-cross побайтно.
 * Fast-path: 124 T  (+ call 17 = 141 T итого)
 * ───────────────────────────────────────────────────────────────────── */
static void asm_read_2bytes(void) __naked
{
    __asm
    ld   hl, (_fb_rp)       ;16
    ld   a, (hl)            ; 7  byte1
    ld   (_fb_b1), a        ;13
    inc  hl                 ; 6
    ld   a, h               ; 4
    or   a, l               ; 4
    jr   Z, 161$            ; 7  page-cross after byte1 (rare)
160$:
    ld   a, (hl)            ; 7  byte2
    ld   (_fb_b2), a        ;13
    inc  hl                 ; 6
    ld   a, h               ; 4
    or   a, l               ; 4
    jr   Z, 163$            ; 7  page-cross after byte2 (rare)
162$:
    ld   (_fb_rp), hl       ;16
    ret                     ;10  fast-path 124 T
161$:
    ld   hl, #_fb_cpg       ;10
    inc  (hl)               ;11
    ld   a, (hl)            ; 7
    call _wc_mngcvpl        ;17+
    ld   hl, #0xC000        ;10
    jr   160$               ;12
163$:
    ld   hl, #_fb_cpg       ;10
    inc  (hl)               ;11
    ld   a, (hl)            ; 7
    call _wc_mngcvpl        ;17+
    ld   hl, #0xC000        ;10
    ld   (_fb_rp), hl       ;16
    ret                     ;10
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_emit_skip_spec — запись CMD_SKIP_TICKS + spec_mask в cmd buffer
 *
 * Пишет: [CMD_SKIP_TICKS, val, spec_mask_lo, spec_mask_hi].
 * Обнуляет spec_mask, продвигает fb_wp на 4.
 * Заменяет 6-строчный C-паттерн, встречающийся 9× в fill_buffer/emit_wait.
 *
 * Вход: A = byte1 (tick count минус 1, или литерал).
 * Clobbers: A, C, HL.
 * Стоимость: ~119 T  (+ call 17 = 136 T итого).
 * ───────────────────────────────────────────────────────────────────── */
static void asm_emit_skip_spec(uint8_t val) __naked {
    __asm
    ld   c, a               ; 4  C = val (byte1)
    ld   hl, (_fb_wp)       ;16
    ld   (hl), #0x50        ;10  CMD_SKIP_TICKS = 0x50
    inc  hl                 ; 6
    ld   (hl), c            ; 7  val
    inc  hl                 ; 6
    ld   a, (_spec_mask)    ;13
    ld   (hl), a            ; 7  spec_mask lo
    inc  hl                 ; 6
    ld   a, (_spec_mask+1)  ;13
    ld   (hl), a            ; 7  spec_mask hi
    inc  hl                 ; 6
    ld   (_fb_wp), hl       ;16  advance write pointer
    ;; spec_mask = 0
    ld   hl, #0x0000        ;10
    ld   (_spec_mask), hl   ;16
    ret                     ;10  total: 153 T
    __endasm;
    (void)val;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_emit_opl_b0 — запись CMD_WRITE_B0 + tail-call spectrum_opl_b0
 *
 * Читает fb_b1, fb_b2 (установлены asm_read_2bytes).
 * Пишет [CMD_WRITE_B0, reg, val, x] в fb_wp, продвигает fb_wp += 4.
 * Tail-call spectrum_opl_b0(A=reg, L=val) — его RET возвращает
 * управление вызывающему.
 *
 * Стоимость: ~128 T тело + 17 call + 10 jp = 155 T итого.
 * Было ~223 T (SDCC codegen): экономия ~68 T на каждую запись OPL Bank0.
 * ───────────────────────────────────────────────────────────────────── */
static void asm_emit_opl_b0(void) __naked {
    __asm
    ld   hl, (_fb_wp)      ;16
    ld   (hl), #0x40       ;10  CMD_WRITE_B0
    inc  hl                ; 6
    ld   a, (_fb_b1)       ;13
    ld   d, a              ; 4  save reg
    ld   (hl), a           ; 7  [1] = reg
    inc  hl                ; 6
    ld   a, (_fb_b2)       ;13
    ld   (hl), a           ; 7  [2] = val
    inc  hl                ; 6
    inc  hl                ; 6  skip [3] padding
    ld   (_fb_wp), hl      ;16  fb_wp += 4
    ld   l, a              ; 4  L = val (fb_b2 still in A)
    ld   a, d              ; 4  A = reg (fb_b1)
    jp   _spectrum_opl_b0  ;10  tail-call
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_emit_opl_b1 — запись CMD_WRITE_B1 + tail-call spectrum_opl_b1
 * Аналогично asm_emit_opl_b0, но для OPL3 Bank 1.
 * ───────────────────────────────────────────────────────────────────── */
static void asm_emit_opl_b1(void) __naked {
    __asm
    ld   hl, (_fb_wp)      ;16
    ld   (hl), #0x80       ;10  CMD_WRITE_B1
    inc  hl                ; 6
    ld   a, (_fb_b1)       ;13
    ld   d, a              ; 4
    ld   (hl), a           ; 7
    inc  hl                ; 6
    ld   a, (_fb_b2)       ;13
    ld   (hl), a           ; 7
    inc  hl                ; 6
    inc  hl                ; 6
    ld   (_fb_wp), hl      ;16
    ld   l, a              ; 4
    ld   a, d              ; 4
    jp   _spectrum_opl_b1  ;10  tail-call
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_emit_saa — SAA1099 0xBD handler (inline asm)
 *
 * Вызывается ПОСЛЕ asm_read_2bytes() (fb_b1 = reg, fb_b2 = val).
 *
 * Логика:
 *   fb_is_vgm==0 (cmdblk) → emit raw CMD_WRITE_SAA, return
 *   VGM mode:
 *     bit7=1 (chip1):
 *       cfg_saa_mode==0 → skip (return)
 *       cfg_saa_mode==1 → strip bit7, emit, spectrum_saa
 *       cfg_saa_mode==2 → emit raw, spectrum_saa2
 *     bit7=0 (chip0):
 *       cfg_saa_mode==1 → skip (return)
 *       else → emit raw, spectrum_saa
 *
 * CMD_WRITE_SAA = 0x60.  spectrum_saa/saa2: A=reg, L=val.
 * Clobbers: A, B, C, D, E, H, L.
 * ───────────────────────────────────────────────────────────────────── */
static void asm_emit_saa(void) __naked {
    __asm

    ;; D = reg (fb_b1), E = val (fb_b2)
    ld   a, (_fb_b1)       ;13
    ld   d, a              ; 4
    ld   a, (_fb_b2)       ;13
    ld   e, a              ; 4

    ;; cmdblk (fb_is_vgm==0) → emit raw, no spectrum
    ld   a, (_fb_is_vgm)   ;13
    or   a, a              ; 4
    jr   nz, 200$          ;12/7
    call 250$
    ret

200$:
    ;; VGM mode — A = cfg_saa_mode
    ld   a, (_cfg_saa_mode) ;13
    bit  7, d              ; 8
    jr   nz, 210$          ;12/7  chip 1

    ;; ── Chip 0: skip if mode==1 ──
    cp   a, #1             ; 7
    ret  z                 ; skip chip0 in mode 1
    jr   230$              ; → emit + spectrum_saa

210$:
    ;; ── Chip 1: skip if mode==0 ──
    or   a, a              ; 4
    ret  z                 ; skip chip1 in mode 0
    cp   a, #2             ; 7
    jr   z, 220$           ; → turbo

    ;; mode 1: strip bit7 → play on chip 0
    res  7, d              ; 8
    ;; fall through to 230$ (emit + spectrum_saa)

230$:
    ;; emit D,E → spectrum_saa(A=reg, L=val)
    call 250$
    ld   a, d              ; 4
    ld   l, e              ; 4
    jp   _spectrum_saa     ;10

220$:
    ;; turbo: emit raw → spectrum_saa2(A=reg&0x7F, L=val)
    call 250$
    ld   a, d              ; 4
    and  a, #0x7F          ; 7
    ld   l, e              ; 4
    jp   _spectrum_saa2    ;10

    ;; ── shared: write [0x60, D, E, pad] to fb_wp ──
250$:
    ld   hl, (_fb_wp)      ;16
    ld   (hl), #0x60       ;10  CMD_WRITE_SAA
    inc  hl                ; 6
    ld   (hl), d           ; 7
    inc  hl                ; 6
    ld   (hl), e           ; 7
    inc  hl                ; 6
    inc  hl                ; 6
    ld   (_fb_wp), hl      ;16
    ret                    ;10

    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_scale_fm — FM F-Number масштабирование: fb_scaled = fb_scaled × fm_mul / 7
 *
 * Все используемые FM ratios сводятся к N/7:
 *   fm_mul=0 → bypass (NTSC 45/44 ≈ 1.023, в рамках 5% допуска)
 *   fm_mul=3 → ×3/7  (YM2203 @ 1.5 МГц)
 *   fm_mul=5 → ×5/7  (YM2203 @ 2.5 МГц)
 *   fm_mul=6 → ×6/7  (YM2203 @ 3 МГц)
 *   fm_mul=8 → ×8/7  (YM2203 @ 4 МГц)
 *
 * Умножение: shifts + adds (~30 T).
 * Деление на 7: binary long division 16 итераций (~770 T).
 * Итого ~840 T — против ~1200 T у SDCC __muluint + __divuint.
 *
 * Вход:  fb_scaled = fnum (11-bit, 0..2047).
 * Выход: fb_scaled = fnum × fm_mul / 7.
 * Clobbers: A, B, D, E, H, L.
 * ───────────────────────────────────────────────────────────────────── */
static void asm_scale_fm(void) __naked {
    __asm
    ld   a, (_fm_mul)
    or   a
    ret  z                  ; fm_mul=0 → bypass

    ld   hl, (_fb_scaled)   ; HL = fnum

    ;; ── Умножение fnum x N (dispatch по fm_mul) ──
    cp   #3
    jr   z, sfm_mul3
    cp   #5
    jr   z, sfm_mul5
    cp   #6
    jr   z, sfm_mul6
    ;; fm_mul=8: x8
    add  hl, hl             ; x2
    add  hl, hl             ; x4
    add  hl, hl             ; x8
    jr   sfm_div7

sfm_mul3:
    ld   d, h
    ld   e, l               ; DE = fnum
    add  hl, hl             ; x2
    add  hl, de             ; x3
    jr   sfm_div7

sfm_mul5:
    ld   d, h
    ld   e, l               ; DE = fnum
    add  hl, hl             ; x2
    add  hl, hl             ; x4
    add  hl, de             ; x5
    jr   sfm_div7

sfm_mul6:
    ld   d, h
    ld   e, l               ; DE = fnum
    add  hl, hl             ; x2
    add  hl, de             ; x3
    add  hl, hl             ; x6
    ;; fall through

    ;; ── Деление HL на 7, результат в HL ──────────
    ;; Binary long division: 16-bit dividend, 8-bit divisor.
    ;; Остаток в A (всегда 0..6, помещается в 8 бит).
    ;; Максимум: 2047x8 = 16376; 16376/7 = 2339.
sfm_div7:
    xor  a                  ; A = remainder = 0
    ld   b, #16             ; 16 бит
sfm_dloop:
    add  hl, hl             ; shift dividend/quotient left, MSB → CF
    rla                     ; CF → remainder bit 0
    cp   #7                 ; remainder >= 7?
    jr   c, sfm_dskip       ; нет → пропуск
    sub  #7                 ; remainder -= 7
    inc  l                  ; set quotient bit
sfm_dskip:
    djnz sfm_dloop          ; next bit

    ld   (_fb_scaled), hl   ; store result
    ret
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * emit_wait — вставить паузу tk тиков (ISR)
 *
 * Использует два механизма паузы:
 *   CMD_SKIP_TICKS(N)  — N=0..ISR_TICKS_PER_FRAME-1, пауза N+1 тик.
 *                         ISR не вызывается (экономия CPU for main loop).
 *   CMD_WAIT(val)       — обычная пауза val+1 тик (ISR функционирует).
 *
 * ИНВАРИАНТ: значение N для CMD_SKIP_TICKS строго < ISR_TICKS_PER_FRAME.
 * N ≥ ISR_TICKS_PER_FRAME → полный оборот pos_table → ISR промахивается
 * мимо своей INT-позиции → каскадный пропуск кадров, падение темпа ×10+.
 *
 * Стратегия:
 *   Для пауз ≤ ISR_TICKS_PER_FRAME: CMD_SKIP_TICKS (1 команда, ISR молчит)
 *   Для пауз > ISR_TICKS_PER_FRAME: CMD_WAIT(основная часть) + CMD_SKIP_TICKS(≤ ISR_TICKS_PER_FRAME-1)
 *   Перед секундным рубежом: аналогично + CMD_INC_SEC
 *
 * Макс. расход буфера на один вызов (для tk≤4095):
 *   2 sec boundary × 12 + remainder 8 = 32 байт
 * Зазор: вызывающий обязан обеспечить off < CMD_BUF_SIZE - 48
 * ───────────────────────────────────────────────────────────────────── */
static void emit_wait(uint16_t tk)
{
    while (tk > 0)
    {
        /* ── SEC-рубеж: бюджет помещается в остаток ─────────── */
        if (vgm_sec_budget <= tk)
        {
            fb_w = vgm_sec_budget;

            if (fb_w > ISR_TICKS_PER_FRAME) {
                /* Большой бюджет: CMD_WAIT(основная часть) + CMD_SKIP_TICKS */
                fb_w -= ISR_TICKS_PER_FRAME;
                fb_wp[0] = CMD_WAIT;
                fb_wp[1] = (uint8_t)(fb_w - 1);
                fb_wp[2] = (uint8_t)((fb_w - 1) >> 8);
                fb_wp += 4;
                asm_emit_skip_spec(ISR_TICKS_PER_FRAME - 1);
            } else if (fb_w >= 1) {
                asm_emit_skip_spec((uint8_t)(fb_w - 1));
            }
            /* CMD_INC_SEC */
            fb_wp[0] = CMD_INC_SEC;
            fb_wp += 4;

            tk -= vgm_sec_budget;
            vgm_sec_budget = ISR_FREQ;
            continue;
        }

        /* ── Остаток: tk < sec_budget ───────────────────────── */
        if (tk > ISR_TICKS_PER_FRAME) {
            /* Более кадра: CMD_WAIT(основная часть) + CMD_SKIP_TICKS(остаток) */
            fb_w = tk - ISR_TICKS_PER_FRAME;
            fb_wp[0] = CMD_WAIT;
            fb_wp[1] = (uint8_t)(fb_w - 1);
            fb_wp[2] = (uint8_t)((fb_w - 1) >> 8);
            fb_wp += 4;
            vgm_sec_budget -= fb_w;
            tk = ISR_TICKS_PER_FRAME;
        }
        /* Последние ≤ISR_TICKS_PER_FRAME тиков через SKIP (экономия CPU) */
        asm_emit_skip_spec((uint8_t)(tk - 1));
        vgm_sec_budget -= tk;
        tk = 0;
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * vgm_fill_buffer  (v5 — HL queue integrated)
 *
 * Заполняет один командный буфер ISR целиком (512 байт), управляя
 * переходами между высокоуровневыми командами (HL queue):
 *
 *   HLCMD_CMDBLK  — подключает страницу cmdblocks, парсит VGM-опкоды
 *                   блока в ISR-команды; при 0x66 переходит к следующей
 *                   HL-команде и продолжает заполнять ТОТ ЖЕ буфер.
 *   HLCMD_PLAY    — воспроизводит VGM из мегабуфера; при заполнении
 *                   буфера сохраняет состояние и выходит; при 0x66
 *                   переходит дальше по очереди.
 *   HLCMD_LOOP    — перематывает к loop-точке, морфирует в PLAY.
 *   HLCMD_ISR_DONE— пишет CMD_ISR_DONE в буфер, выходит досрочно.
 *
 * EOF-проверка удалена из горячего цикла — при загрузке в конец
 * VGM-данных вписывается 0x66-сентинел (см. main.c write_vgm_sentinel).
 * ───────────────────────────────────────────────────────────────────── */

void vgm_fill_buffer(uint8_t buf_idx)
{
    fb_buf = buf_idx ? cmd_buf_b : cmd_buf_a;
    fb_wp  = fb_buf;
    fb_end = fb_buf + (CMD_BUF_SIZE - 48);

    /* Пауза — заполнить тишиной */
    if (vgm_paused) {
        fb_buf[0] = CMD_SKIP_TICKS; fb_buf[1] = ISR_TICKS_PER_FRAME - 1;
        fb_buf[4] = CMD_END_BUF;
        return;
    }

    fb_wacc = vgm_wait_accum;

    /* ── Макрос: проверка границы страницы после fb_rp++ ───────────
     * При inc HL через #FFFF -> #0000 переключаем VPL-страницу.
     * Fast-path (99.99 %): одна проверка HL==0, ~15 T-states.        */
#define PAGE_CHK() do { \
    if (!(uint16_t)(fb_rp)) { fb_cpg++; wc_mngcvpl(fb_cpg); fb_rp = (uint8_t *)0xC000; } \
} while(0)

    /* ═══════════════════════════════════════════════════════════════
     * next_hl — диспетчер высокоуровневых команд
     * Вызывается при входе и при каждом 0x66 (конец блока/файла).
     * НЕ выходит из функции — продолжает заполнять тот же буфер.
     * ═══════════════════════════════════════════════════════════════ */
next_hl:
    if (vgm_hl_pos >= vgm_hl_len) {
        /* Очередь исчерпана — заполнить остаток тишиной */
        if (fb_wp == fb_buf) {
            fb_buf[0] = CMD_SKIP_TICKS; fb_buf[1] = ISR_TICKS_PER_FRAME - 1;
            fb_wp = fb_buf + 4;
        }
        goto finish;
    }

    fb_e = &vgm_hl_queue[vgm_hl_pos];

    // обработка высокоуровневой команды — установка fb_rp, fb_cpg, fb_is_vgm
    switch (fb_e->cmd) {

    case HLCMD_CMDBLK:
        /* Подключить страницу cmdblocks, найти адрес блока */
        wc_mngc_pl(CMDBLK_PAGE);
        fb_rp  = (uint8_t *)((const uint16_t *)0xC000)[fb_e->param];
        fb_cpg = 0;       /* не используется для cmdblk, PAGE_CHK не сработает */
        fb_is_vgm = 0;
        break;

    case HLCMD_PLAY:
        fb_cpg  = vgm_cur_page;
        fb_rp   = (uint8_t *)vgm_read_ptr;
        wc_mngcvpl(fb_cpg);
        /* Переподключить LUT: WC API (print_line и т.д.) мог перемапить Window 0 */
        if (vgm_freq_mode == FREQ_MODE_TABLE)
            wc_mng0_pl(freq_lut_page);
        fb_is_vgm = 1;
        break;

    case HLCMD_LOOP:
        if (!vgm_rewind_to_loop()) {
            /* Нет loop-точки — пропустить */
            vgm_hl_pos++;
            goto next_hl;
        }
        vgm_loop_count++;
        fb_e->cmd = HLCMD_PLAY;   /* чтобы не перематывать снова */
        fb_cpg  = vgm_cur_page;
        fb_rp   = (uint8_t *)vgm_read_ptr;
        fb_wacc = vgm_wait_accum;
        wc_mngcvpl(fb_cpg);
        if (vgm_freq_mode == FREQ_MODE_TABLE)
            wc_mng0_pl(freq_lut_page);
        fb_is_vgm = 1;
        break;

    case HLCMD_ISR_DONE:
        fb_wp[0] = CMD_ISR_DONE;
        fb_wp += 4;
        vgm_hl_pos++;
        goto finish;

    default:
        vgm_hl_pos++;
        goto next_hl;
    }

    fb_budget = VGM_FILL_CMD_BUDGET;
    fb_yield_ticks = 0;

    /* ═══════════════════════════════════════════════════════════════
     * Горячий цикл — разбор VGM-опкодов → ISR-команды
     * EOF-проверка удалена (гарантирован 0x66 в данных).
     * ═══════════════════════════════════════════════════════════════ */
    while (fb_wp < fb_end) // зазор 48 байт для emit_wait (макс. 32 байта) + CMD_END_BUF (4 байта) + safety
    {
        /* ── Читаем опкод ──────────────────────────────────────── */
        fb_op = asm_read_byte();

        /* ═══════ OPL2 запись (0x5A) — самая частая ═══════ */
        if (fb_op == 0x5A)
        {
            asm_read_2bytes();
            asm_emit_opl_b0();
            goto do_budget;
        }

        /* ═══════ OPL3 Bank 1 (0x5F) ═══════ */
        if (fb_op == 0x5F)
        {
            asm_read_2bytes();
            asm_emit_opl_b1();
            goto do_budget;
        }

        /* ═══════ OPL Bank 0: 0x5B(YM3526) / 0x5C(Y8950) / 0x5E(OPL3bk0) ═══════
         * Диапазон 0x5B..0x5E — одна проверка, все → bank0.
         * 0x5D (YMZ280B) не поддерживается — если встретится, запись в bank0 безвредна. */
        if ((uint8_t)(fb_op - 0x5B) <= (0x5E - 0x5B))
        {
            asm_read_2bytes();
            asm_emit_opl_b0();
            goto do_budget;
        }

        /* ═══════ Frame wait 1/60 (0x62) ═══════ */
        if (fb_op == 0x62)
        { asm_wait_62(); goto do_wait; }

        /* ═══════ Frame wait 1/50 (0x63) ═══════ */
        if (fb_op == 0x63)
        { asm_wait_63(); goto do_wait; }

        /* ═══════ Короткая пауза 1-16 samples (0x70-0x7F) ═══════ */
#ifndef VGM_NO_SHORT_WAITS
        if ((fb_op & 0xF0) == 0x70)
        { asm_short_wait(); goto do_wait; }
#else
        /* VGM_NO_SHORT_WAITS: пропуск коротких пауз 0x70-0x7F */
        if ((fb_op & 0xF0) == 0x70) continue;
#endif

        /* ═══════ Произвольная пауза (0x61) ═══════ */
        if (fb_op == 0x61)
        {
            asm_read_2bytes();
            asm_arb_wait();
            goto do_wait;
        }

        /* ═══════ YM2203 SSG+FM (0x55) ═══════ */
        if (fb_op == 0x55)
        {
            asm_read_2bytes();
            spectrum_ay(fb_b1, fb_b2);
          if (vgm_freq_mode != FREQ_MODE_NATIVE) {
            /* ── PSG tone period (reg 0x00-0x05): 12-bit, парами lo/hi ── */
            if (fb_b1 <= 0x05) {
                fb_ch = fb_b1 >> 1;           /* 0,1,2 */
                if (fb_b1 & 1) {
                    /* hi-nibble → обновить shadow, пересчитать полные 12 бит */
                    psg_shadow[fb_ch] = (psg_shadow[fb_ch] & 0x00FF)
                                      | ((uint16_t)(fb_b2 & 0x0F) << 8);
                } else {
                    /* lo-byte → обновить shadow */
                    psg_shadow[fb_ch] = (psg_shadow[fb_ch] & 0x0F00) | fb_b2;
                }
                fb_scaled = freq_lut_base[psg_shadow[fb_ch]];
                if (fb_scaled > 0x0FFF) fb_scaled = 0x0FFF;
                /* Отправляем оба lo+hi: scaled меняет оба байта одновременно */
                fb_wp[0] = CMD_WRITE_AY;
                fb_wp[1] = fb_ch << 1;  /* reg = 0,2,4 */
                fb_wp[2] = (uint8_t)(fb_scaled & 0xFF);
                fb_wp += 4;
                fb_wp[0] = CMD_WRITE_AY;
                fb_wp[1] = (fb_ch << 1) | 1;  /* reg = 1,3,5 */
                fb_wp[2] = (uint8_t)((fb_scaled >> 8) & 0x0F);
                fb_wp += 4;
                goto do_budget;
            }
            /* ── Noise period (reg 0x06): 5-bit, clamp ── */
            else if (fb_b1 == 0x06) {
                fb_b2 = (uint8_t)freq_lut_base[fb_b2 & 0x1F];
                if (fb_b2 > 31) fb_b2 = 31;
            }
            /* ── FM F-Number (regs 0xA0-0xA2 lo, 0xA4-0xA6 hi+block) ── */
            else if (fm_mul && fb_b1 >= 0xA0 && fb_b1 <= 0xA6) {
                uint8_t fmr = fb_b1 - 0xA0;  /* 0..6 */
                if (fmr <= 2) {
                    fb_ch = fmr;
                    fm_fnum[fb_ch] = (fm_fnum[fb_ch] & 0x0700) | fb_b2;
                } else if (fmr >= 4) {
                    fb_ch = fmr - 4;
                    fm_block[fb_ch] = (fb_b2 >> 3) & 0x07;
                    fm_fnum[fb_ch] = (fm_fnum[fb_ch] & 0x00FF)
                                   | ((uint16_t)(fb_b2 & 0x07) << 8);
                } else {
                    goto ym1_fm_passthru;  /* 0xA3 */
                }
                {
                    uint8_t blk = fm_block[fb_ch];
                    fb_scaled = fm_fnum[fb_ch];
                    asm_scale_fm();
                    if (fb_scaled > 0x7FF) {
                        fb_scaled >>= 1;
                        if (fb_scaled > 0x7FF) fb_scaled = 0x7FF;
                        blk++;
                        if (blk > 7) blk = 7;
                    }
                    fb_wp[0] = CMD_WRITE_AY;
                    fb_wp[1] = 0xA4 + fb_ch;
                    fb_wp[2] = (blk << 3) | (uint8_t)((fb_scaled >> 8) & 0x07);
                    fb_wp += 4;
                    if (fmr <= 2) {
                        fb_wp[0] = CMD_WRITE_AY;
                        fb_wp[1] = 0xA0 + fb_ch;
                        fb_wp[2] = (uint8_t)(fb_scaled & 0xFF);
                        fb_wp += 4;
                    }
                    goto do_budget;
                }
            }
ym1_fm_passthru:;
          }
            fb_wp[0] = CMD_WRITE_AY;
            fb_wp[1] = fb_b1; fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ YM2203 #2 SSG+FM (0xA5) ═══════
         * VGM dual chip: 0x55 → 0xA5 (последняя цифра 0x5n → 0xAn) */
        if (fb_op == 0xA5)
        {
            asm_read_2bytes();
            spectrum_ay2(fb_b1, fb_b2);
          if (vgm_freq_mode != FREQ_MODE_NATIVE) {
            if (fb_b1 <= 0x05) {
                fb_ch = fb_b1 >> 1;
                if (fb_b1 & 1) {
                    psg_shadow2[fb_ch] = (psg_shadow2[fb_ch] & 0x00FF)
                                       | ((uint16_t)(fb_b2 & 0x0F) << 8);
                } else {
                    psg_shadow2[fb_ch] = (psg_shadow2[fb_ch] & 0x0F00) | fb_b2;
                }
                fb_scaled = freq_lut_base[psg_shadow2[fb_ch]];
                if (fb_scaled > 0x0FFF) fb_scaled = 0x0FFF;
                fb_wp[0] = CMD_WRITE_AY2;
                fb_wp[1] = fb_ch << 1;
                fb_wp[2] = (uint8_t)(fb_scaled & 0xFF);
                fb_wp += 4;
                fb_wp[0] = CMD_WRITE_AY2;
                fb_wp[1] = (fb_ch << 1) | 1;
                fb_wp[2] = (uint8_t)((fb_scaled >> 8) & 0x0F);
                fb_wp += 4;
                goto do_budget;
            }
            else if (fb_b1 == 0x06) {
                fb_b2 = (uint8_t)freq_lut_base[fb_b2 & 0x1F];
                if (fb_b2 > 31) fb_b2 = 31;
            }
            /* ── FM F-Number чип 2 (regs 0xA0-0xA6) ── */
            else if (fm_mul && fb_b1 >= 0xA0 && fb_b1 <= 0xA6) {
                uint8_t fmr = fb_b1 - 0xA0;
                if (fmr <= 2) {
                    fb_ch = fmr;
                    fm_fnum2[fb_ch] = (fm_fnum2[fb_ch] & 0x0700) | fb_b2;
                } else if (fmr >= 4) {
                    fb_ch = fmr - 4;
                    fm_block2[fb_ch] = (fb_b2 >> 3) & 0x07;
                    fm_fnum2[fb_ch] = (fm_fnum2[fb_ch] & 0x00FF)
                                    | ((uint16_t)(fb_b2 & 0x07) << 8);
                } else {
                    goto ym2_fm_passthru;
                }
                {
                    uint8_t blk = fm_block2[fb_ch];
                    fb_scaled = fm_fnum2[fb_ch];
                    asm_scale_fm();
                    if (fb_scaled > 0x7FF) {
                        fb_scaled >>= 1;
                        if (fb_scaled > 0x7FF) fb_scaled = 0x7FF;
                        blk++;
                        if (blk > 7) blk = 7;
                    }
                    fb_wp[0] = CMD_WRITE_AY2;
                    fb_wp[1] = 0xA4 + fb_ch;
                    fb_wp[2] = (blk << 3) | (uint8_t)((fb_scaled >> 8) & 0x07);
                    fb_wp += 4;
                    if (fmr <= 2) {
                        fb_wp[0] = CMD_WRITE_AY2;
                        fb_wp[1] = 0xA0 + fb_ch;
                        fb_wp[2] = (uint8_t)(fb_scaled & 0xFF);
                        fb_wp += 4;
                    }
                    goto do_budget;
                }
            }
ym2_fm_passthru:;
          }
            fb_wp[0] = CMD_WRITE_AY2;
            fb_wp[1] = fb_b1; fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ AY8910 dual (0xA0) ═══════ */
        if (fb_op == 0xA0)
        {
            asm_read_2bytes();
            if (fb_b1 & 0x80)
                spectrum_ay2(fb_b1 & 0x7F, fb_b2);
            else
                spectrum_ay(fb_b1 & 0x7F, fb_b2);
          if (vgm_freq_mode != FREQ_MODE_NATIVE) {
            fb_reg = fb_b1 & 0x7F;
            /* PSG tone period (reg 0-5): полный 12-бит через shadow */
            if (fb_reg <= 0x05) {
                fb_ch = fb_reg >> 1;
                if (fb_reg & 1) {
                    psg_shadow[fb_ch] = (psg_shadow[fb_ch] & 0x00FF)
                                      | ((uint16_t)(fb_b2 & 0x0F) << 8);
                } else {
                    psg_shadow[fb_ch] = (psg_shadow[fb_ch] & 0x0F00) | fb_b2;
                }
                fb_scaled = freq_lut_base[psg_shadow[fb_ch]];
                if (fb_scaled > 0x0FFF) fb_scaled = 0x0FFF;
                /* AY (0xA0): отправляем оба байта сразу */
                fb_wp[0] = (fb_b1 & 0x80) ? CMD_WRITE_AY2 : CMD_WRITE_AY;
                fb_wp[1] = fb_ch << 1;  /* lo reg */
                fb_wp[2] = (uint8_t)(fb_scaled & 0xFF);
                fb_wp += 4;
                fb_wp[0] = (fb_b1 & 0x80) ? CMD_WRITE_AY2 : CMD_WRITE_AY;
                fb_wp[1] = (fb_ch << 1) | 1;  /* hi reg */
                fb_wp[2] = (uint8_t)((fb_scaled >> 8) & 0x0F);
                fb_wp += 4;
                goto do_budget;
            }
            /* Noise period (reg 6) */
            else if (fb_reg == 0x06) {
                fb_b2 = (uint8_t)freq_lut_base[fb_b2 & 0x1F];
                if (fb_b2 > 31) fb_b2 = 31;
            }
          }

            fb_wp[0] = (fb_b1 & 0x80) ? CMD_WRITE_AY2 : CMD_WRITE_AY;
            fb_wp[1] = fb_b1 & 0x7F;
            fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ SAA1099 dual (0xBD) ═══════ */
        /* Bit7 of reg byte selects chip (0=chip0, 1=chip1).
         * cfg_saa_mode: 0=chip0 only(default), 1=chip1 on chip0, 2=turbo(both).
         * cmdblk mode (fb_is_vgm==0): bypass filtering, pass raw. */
        if (fb_op == 0xBD)
        {
            asm_read_2bytes();
            asm_emit_saa();
            goto do_budget;
        }

        /* ═══════ Конец данных (0x66) ═══════ */
        if (fb_op == 0x66)
        {
            if (fb_is_vgm) {
                vgm_song_ended = 1;
                vgm_cur_page   = fb_cpg;
                vgm_read_ptr   = (uint16_t)fb_rp;
                vgm_wait_accum = fb_wacc;
            }
            vgm_hl_pos++;
            goto next_hl;
        }

        /* ═══════ Data block (0x67) — редко ═══════ */
        if (fb_op == 0x67)
        {
            fb_rp++; PAGE_CHK();  /* 0x66 */
            fb_rp++; PAGE_CHK();  /* type байт */
            asm_read_2bytes();
            fb_t32 = (uint16_t)fb_b1 | ((uint16_t)fb_b2 << 8);
            asm_read_2bytes();
            fb_t32 |= ((uint32_t)fb_b1 << 16) | ((uint32_t)fb_b2 << 24);
            /* сохранить → vgm_skip → восстановить */
            vgm_read_ptr = (uint16_t)fb_rp;
            vgm_cur_page = fb_cpg;
            vgm_skip(fb_t32);
            fb_rp  = (uint8_t *)vgm_read_ptr;
            fb_cpg = vgm_cur_page;
            continue;
        }

        /* ═══════ PCM RAM write (0x68) — редко ═══════ */
        if (fb_op == 0x68)
        {
            vgm_read_ptr = (uint16_t)fb_rp;
            vgm_cur_page = fb_cpg;
            vgm_skip(11);
            fb_rp  = (uint8_t *)vgm_read_ptr;
            fb_cpg = vgm_cur_page;
            continue;
        }

        /* ═══════ DAC Stream (0x90-0x95) — редко ═══════ */
        if (fb_op >= 0x90 && fb_op <= 0x95)
        {
            static const uint8_t dac_len[] = {4, 4, 5, 10, 1, 4};
            fb_n = dac_len[fb_op - 0x90];
            while (fb_n--) { fb_rp++; PAGE_CHK(); }
            continue;
        }

        /* ═══════ Общий пропуск (все остальные команды) ═══════ */
        /* Размеры операндов по VGM spec v1.71.
         * Все явно обработанные опкоды (0x5A,0x5B,..,0xA0,0xBD,
         * 0x61-0x63,0x66-0x68,0x7n,0x90-0x95) сюда не попадают.    */
        {
            fb_n = 0;
            if      (fb_op < 0x30)  fb_n = 0;   /* ниже 0x30 — нет данных  */
            else if (fb_op < 0x40)  fb_n = 1;   /* 0x30-0x3F: reserved 1-op */
            else if (fb_op < 0x4F)  fb_n = 2;   /* 0x40-0x4E: reserved 2-op */
            else if (fb_op <= 0x50) fb_n = 1;   /* 0x4F GG PSG / 0x50 SN76489 */
            else if (fb_op < 0x60)  fb_n = 2;   /* 0x51-0x5F: chip writes 2-op */
            else if (fb_op == 0x64) fb_n = 3;   /* 0x64: wait override (cc nn nn) */
            else if (fb_op < 0xA0)  fb_n = 0;   /* 0x8n DAC+wait, прочее 0x6x/9x */
            else if (fb_op < 0xC0)  fb_n = 2;   /* 0xA0-0xBF: chip writes 2-op */
            else if (fb_op < 0xE0)  fb_n = 3;   /* 0xC0-0xDF: chip writes 3-op */
            else                    fb_n = 4;   /* 0xE0-0xFF: chip writes 4-op */
            while (fb_n--) { fb_rp++; PAGE_CHK(); }
        }
        continue;

    /* ═══════ Обработка задержек (wait paths устанавливают fb_tk и fb_wacc напрямую) ═══════ */
    do_wait:
        /* Компенсация: бюджетные yield-ы уже потратили реальные ISR-тики.
         * Вычитаем их из VGM-задержки, чтобы общий темп не замедлялся. */
        if (fb_tk > fb_yield_ticks)
            // реальная пауза меньше заявленной на fb_yield_ticks, компенсируем
            fb_tk -= fb_yield_ticks; 
        else
            // пауза полностью съедена накладными расходами, не ждем вообще
            fb_tk = 0;
        
        // сбросить счетчик накладных расходов, они уже компенсированы
        fb_yield_ticks = 0;

        if (fb_tk) { 
            // есть что ждать

            if (fb_tk <= ISR_TICKS_PER_FRAME && fb_tk < vgm_sec_budget) { 
                // короткая пауза, помещается в один кадр — пропустить через SKIP
                asm_emit_skip_spec((uint8_t)(fb_tk - 1));
                vgm_sec_budget -= fb_tk; // учесть в сек. бюджете
            } else {
                emit_wait(fb_tk);
            }

            // после паузы бюджет обновлен, можно продолжать заполнять буфер
            fb_budget = VGM_FILL_CMD_BUDGET;
        }
        continue;

    do_budget:
        // Бюджет команд: после каждого опкода уменьшаем, при 0 — пауза или yield
        if (--fb_budget == 0)
        {
            // бюджет исчерпан — сделать паузу, восстановить бюджет

            if (vgm_sec_budget > 1) {
                // Остаток бюджета больше одного кадра — сделать yield (пауза через CMD_INC_SEC)
                asm_emit_skip_spec(0);
                vgm_sec_budget--;
                fb_yield_ticks++;  /* запомнить: этот тик — накладной расход */
            } else {
                // Остаток бюджета меньше одного кадра — сделать реальную паузу
                emit_wait(1);
            }
            fb_budget = VGM_FILL_CMD_BUDGET;
        }
    }

#undef PAGE_CHK

    /* Буфер заполнен — сохранить VGM-состояние если читали файл */
    if (fb_is_vgm) {
        vgm_cur_page    = fb_cpg;
        vgm_read_ptr    = (uint16_t)fb_rp;
        vgm_wait_accum  = fb_wacc;
    }

finish:
    /* Принудительная пауза перед CMD_END_BUF — гарантирует, что ISR
     * не обработает хвост этого буфера + голову следующего за один тик
     * (иначе пропуск INT-позиции и падение темпа в десятки раз).       */
    asm_emit_skip_spec(0);

    fb_wp[0] = CMD_END_BUF;
}

/* ── Буферы GD3 metadata ─────────────────────────────────────────── */
char vgm_gd3_track[VGM_GD3_LEN];
char vgm_gd3_game[VGM_GD3_LEN];
char vgm_gd3_system[VGM_GD3_LEN];
char vgm_gd3_author[VGM_GD3_LEN];

/* ─────────────────────────────────────────────────────────────────────
 * vgm_parse_gd3 — разбор GD3-тега (UTF-16LE → ASCII, только English)
 *
 * GD3 v1.00 содержит 11 UTF-16LE строк:
 *   0: Track title (EN)    1: Track title (JP)
 *   2: Game name   (EN)    3: Game name   (JP)
 *   4: System name (EN)    5: System name (JP)
 *   6: Author      (EN)    7: Author      (JP)
 *   8: Date                9: Ripper        10: Notes
 *
 * Нам нужны только EN: индексы 0, 2, 4, 6.
 * ───────────────────────────────────────────────────────────────────── */
void vgm_parse_gd3(void)
{
    uint8_t *base = (uint8_t *)0xC000;
    uint32_t gd3_off_field;
    uint32_t gd3_abs;
    uint8_t  page;
    uint16_t ptr;
    uint8_t  str_idx;
    char    *dst;
    uint8_t  dst_max;
    uint8_t  dst_pos;

    /* Очистить все поля */
    vgm_gd3_track[0]  = '\0';
    vgm_gd3_game[0]   = '\0';
    vgm_gd3_system[0] = '\0';
    vgm_gd3_author[0] = '\0';

    /* Страница 0 для чтения заголовка */
    wc_mngcvpl(0);
    base = (uint8_t *)0xC000;

    /* GD3 offset: 32-bit LE в 0x14, относительно 0x14 */
    gd3_off_field = (uint32_t)base[0x14]
                  | ((uint32_t)base[0x15] << 8)
                  | ((uint32_t)base[0x16] << 16)
                  | ((uint32_t)base[0x17] << 24);
    if (gd3_off_field == 0)
        return;

    gd3_abs = 0x14UL + gd3_off_field;

    /* Переключаемся на страницу с GD3 */
    page = (uint8_t)(gd3_abs >> 14);
    wc_mngcvpl(page);
    base = (uint8_t *)0xC000;
    ptr = (uint16_t)(gd3_abs & 0x3FFF);

    /* Проверяем сигнатуру "Gd3 " */
    if (base[ptr] != 'G' || base[ptr+1] != 'd' ||
        base[ptr+2] != '3' || base[ptr+3] != ' ')
        return;

    /* Пропускаем: signature(4) + version(4) + data_length(4) = 12 */
    ptr += 12;
    if (ptr >= 0x4000) { page++; wc_mngcvpl(page); ptr -= 0x4000; }

    /* Читаем UTF-16LE строки, нам нужны EN: #0, #2, #4, #6 */
    for (str_idx = 0; str_idx < 8; str_idx++) {
        /* Выбираем буфер для EN-строк (чётные) */
        dst = 0;
        dst_max = 0;
        if (str_idx == 0) { dst = vgm_gd3_track;  dst_max = VGM_GD3_LEN - 1; }
        if (str_idx == 2) { dst = vgm_gd3_game;   dst_max = VGM_GD3_LEN - 1; }
        if (str_idx == 4) { dst = vgm_gd3_system;  dst_max = VGM_GD3_LEN - 1; }
        if (str_idx == 6) { dst = vgm_gd3_author;  dst_max = VGM_GD3_LEN - 1; }
        dst_pos = 0;

        /* Читаем UTF-16LE до нулевого терминатора 0x0000 */
        for (;;) {
            uint8_t lo, hi;
            lo = base[ptr];
            hi = base[ptr + 1];
            ptr += 2;
            if (ptr >= 0x4000) { page++; wc_mngcvpl(page); ptr -= 0x4000; }

            if (lo == 0 && hi == 0)
                break;  /* конец строки */

            /* ASCII: берём только lo если hi==0 и lo < 128 */
            if (dst && dst_pos < dst_max) {
                if (hi == 0 && lo >= 0x20 && lo < 0x7F)
                    dst[dst_pos++] = (char)lo;
                else
                    dst[dst_pos++] = '?';  /* non-ASCII → '?' */
            }
        }
        if (dst)
            dst[dst_pos] = '\0';
    }
}

/* ─────────────────────────────────────────────────────────────────────
 * vgm_rewind_to_loop
 * Перематывает к loop-точке, если она есть, и сбрасывает флаг окончания песни.
 * Возвращает 1 если перемотано, 0 если loop-точки нет.
 * Loop-точка устанавливается при загрузке VGM-файла, если в нём есть loop_offset.
 * Loop-точка — абсолютный адрес в VGM-данных, который может быть в любом месте файла
 * (обычно после заголовка, но может быть и в середине данных).
 * При перемотке к loop-точке нужно восстановить состояние чтения (текущая страница, указатель чтения, накопленные паузы),
 * чтобы продолжить воспроизведение оттуда без сбоев.
 * ───────────────────────────────────────────────────────────────────── */
uint8_t vgm_rewind_to_loop(void)
{
    if (!vgm_loop_addr)
        return 0;

    vgm_cur_page = vgm_loop_page;
    vgm_read_ptr = vgm_loop_addr;
    vgm_song_ended = 0;
    vgm_wait_accum = 0;
    vgm_sec_budget = ISR_FREQ;
    wc_mngcvpl(vgm_cur_page);
    return 1;
}

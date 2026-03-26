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
 *   SAA1099:        cmd 0xBD → CMD_WRITE_SAA / CMD_WRITE_SAA2 (dual)
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
#ifdef VGM_FREQ_SCALE
#include "../inc/freq_lut_map.h"
#endif

/* ── Состояние парсера ──────────────────────────────────────────────── */
volatile uint8_t vgm_song_ended;
uint8_t vgm_paused;
uint8_t vgm_cur_page;
uint16_t vgm_read_ptr;
uint8_t vgm_end_page;
uint16_t vgm_end_addr;
uint16_t vgm_version;
uint8_t vgm_chip_type;
uint8_t vgm_chip_count;
vgm_chip_entry_t vgm_chip_list[VGM_MAX_CHIPS];
uint16_t vgm_loop_addr;
uint8_t vgm_loop_page;


/* ── Аккумулятор остатка задержки (VGM samples, 0..15) ─────────────── */
static uint16_t vgm_wait_accum = 0;

#ifdef VGM_FREQ_SCALE
/* ── Масштабирование частот PSG/FM (только LUT) ─────────────────────── */
uint8_t vgm_freq_mode;
uint16_t *freq_lut_base;

/* Буфер hi-байтов FM F-Number: хранит последний записанный 0xA4/A5/A6
 * (Block[2:0] | F-Num[10:8]).  Записывается при write 0xA4-0xA6,
 * используется при защёлке через 0xA0-0xA2. */
static uint8_t fm_fnum_hi[3];   /* каналы 0,1,2 */

/* Shadow-регистры PSG: полные 12-бит tone period на канал.
 * Обновляются при записи lo/hi, используются для LUT lookup. */
static uint16_t psg_shadow[3];  /* каналы 0,1,2 */
#endif /* VGM_FREQ_SCALE */

/* ── Бюджеты для вставки служебных ISR-команд ──────────────────────── */
static uint16_t vgm_sec_budget = ISR_FREQ;  /* тиков до CMD_INC_SEC  */


/* ── Лимит команд между принудительными yield ──────────────────────── */
#define VGM_FILL_CMD_BUDGET 32u


/* ── Chip scan table (ROM-const) ────────────────────────────────────── */
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

#ifdef VGM_FREQ_SCALE
    /* ── Расчёт масштабов частот PSG и FM ─────────────────────────────
     * Ищем clock YM2203 (cid 6) или AY8910 (cid 18) в chip_list.
     * FM-часть YM2203 работает на полной частоте чипа,
     * PSG-часть YM2203 = fclk/2, AY8910 = fclk как есть. */
    vgm_freq_mode = FREQ_MODE_NATIVE;
    freq_lut_base = (uint16_t *)0;
    fm_fnum_hi[0] = 0; fm_fnum_hi[1] = 0; fm_fnum_hi[2] = 0;
    psg_shadow[0] = 0; psg_shadow[1] = 0; psg_shadow[2] = 0;
    for (i = 0; i < vgm_chip_count; i++) {
        uint16_t psg_khz;
        uint32_t fc = (uint32_t)vgm_chip_list[i].clock_khz * 1000UL;
        if (vgm_chip_list[i].id == VGM_OFF_YM2203) {
            psg_khz = (uint16_t)(fc / 2000UL);  /* SSG = fclk/2 */
        } else if (vgm_chip_list[i].id == VGM_OFF_AY8910) {
            psg_khz = vgm_chip_list[i].clock_khz;
        } else {
            continue;
        }

        /* ── Определяем режим масштабирования ─────────────────────
         * 1) bypass: |psg_khz - HW| ≤ 2% → native
         * 2) LUT table match (±5%) → table mode (Window 0)
         * Нет match → остаётся native (экзотика <1% файлов) */
        {
            uint16_t hw_khz = FREQ_LUT_HW_KHZ;
            uint16_t diff = (psg_khz > hw_khz)
                          ? (psg_khz - hw_khz)
                          : (hw_khz - psg_khz);
            if (diff <= FREQ_LUT_BYPASS_TOL) {
                /* ±2% — native, no scaling needed */
                vgm_freq_mode = FREQ_MODE_NATIVE;
            } else {
                /* Ищем подходящую предрасчитанную таблицу (±5%) */
                uint8_t j;
                for (j = 0; j < FREQ_LUT_COUNT; j++) {
                    uint16_t d = (psg_khz > freq_lut_map[j].clk_khz)
                               ? (psg_khz - freq_lut_map[j].clk_khz)
                               : (freq_lut_map[j].clk_khz - psg_khz);
                    if (d <= freq_lut_map[j].tol_khz) {
                        /* Матч! Подключаем LUT страницу в Window 0 */
                        wc_mng0_pl(freq_lut_map[j].page);
                        freq_lut_base = (uint16_t *)freq_lut_map[j].offset;
                        vgm_freq_mode = FREQ_MODE_TABLE;
                        break;
                    }
                }
                /* Нет match → native (без масштабирования) */
            }
        }
        break;
    }
#endif

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
 * emit_wait — вставить паузу tk тиков (ISR 2734 Hz)
 *
 * Использует два механизма паузы:
 *   CMD_SKIP_TICKS(N)  — N=0..ISR_TICKS_PER_FRAME-1, пауза N+1 тик.  ISR не вызывается
 *                         в промежутке (экономия CPU для main loop).
 *   CMD_WAIT(val)       — обычная пауза val+1 тик (ISR функционирует).
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
static void emit_wait(uint8_t *buf, uint16_t *poff, uint16_t tk)
{
    uint16_t off = *poff;

    while (tk > 0)
    {
        /* ── SEC-рубеж: бюджет помещается в остаток ─────────── */
        if (vgm_sec_budget <= tk)
        {
            uint16_t ns = vgm_sec_budget;

            if (ns > ISR_TICKS_PER_FRAME) {
                /* Большой бюджет: CMD_WAIT(основная часть) + CMD_SKIP_TICKS */
                uint16_t w = ns - ISR_TICKS_PER_FRAME;
                buf[off] = CMD_WAIT;
                buf[off+1] = (uint8_t)(w - 1);
                buf[off+2] = (uint8_t)((w - 1) >> 8);
                buf[off+3] = 0;
                off += 4;
                buf[off] = CMD_SKIP_TICKS;
                buf[off+1] = ISR_TICKS_PER_FRAME - 1;
                buf[off+2] = 0; buf[off+3] = 0;
                off += 4;
            } else if (ns >= 1) {
                buf[off] = CMD_SKIP_TICKS;
                buf[off+1] = (uint8_t)(ns - 1);
                buf[off+2] = 0; buf[off+3] = 0;
                off += 4;
            }
            /* CMD_INC_SEC */
            buf[off] = CMD_INC_SEC;
            buf[off+1] = 0; buf[off+2] = 0; buf[off+3] = 0;
            off += 4;

            tk -= vgm_sec_budget;
            vgm_sec_budget = ISR_FREQ;
            continue;
        }

        /* ── Остаток: tk < sec_budget ───────────────────────── */
        if (tk > ISR_TICKS_PER_FRAME) {
            /* Более кадра: CMD_WAIT(основная часть) + CMD_SKIP_TICKS(остаток) */
            uint16_t w = tk - ISR_TICKS_PER_FRAME;
            buf[off] = CMD_WAIT;
            buf[off+1] = (uint8_t)(w - 1);
            buf[off+2] = (uint8_t)((w - 1) >> 8);
            buf[off+3] = 0;
            off += 4;
            vgm_sec_budget -= w;
            tk = ISR_TICKS_PER_FRAME;
        }
        /* Последние ≤ISR_TICKS_PER_FRAME тиков через SKIP (экономия CPU) */
        buf[off] = CMD_SKIP_TICKS;
        buf[off+1] = (uint8_t)(tk - 1);
        buf[off+2] = 0; buf[off+3] = 0;
        off += 4;
        vgm_sec_budget -= tk;
        tk = 0;
    }

    *poff = off;
}

/* ─────────────────────────────────────────────────────────────────────
 * vgm_fill_buffer  (OPTIMIZED v4 — CMD_SKIP_TICKS + CMD_INC_SEC)
 * ───────────────────────────────────────────────────────────────────── */
void vgm_fill_buffer(uint8_t buf_idx)
{
    uint8_t  *buf;
    uint8_t  *rp;          /* локальный указатель чтения VGM (#C000-#FFFF) */
    uint8_t   cpg;         /* локальная копия vgm_cur_page                 */
    uint8_t   epg;         /* кэш vgm_end_page                             */
    uint16_t  ead;         /* кэш vgm_end_addr                             */
    uint16_t  off;         /* смещение записи в cmd_buf (0..508)           */
    uint8_t   budget;
    uint16_t  wacc;        /* аккумулятор остатка задержки (0..15)         */
    uint8_t   op, b1, b2;

    buf  = buf_idx ? cmd_buf_b : cmd_buf_a;
    cpg  = vgm_cur_page;
    rp   = (uint8_t *)vgm_read_ptr;
    epg  = vgm_end_page;
    ead  = vgm_end_addr;
    wacc = vgm_wait_accum;

    /* Восстановить VPL-страницу VGM (UI мог её переключить) */
    wc_mngcvpl(cpg);

    /* Тишина при паузе или конце */
    if (vgm_song_ended || vgm_paused)
    {
        buf[0] = CMD_SKIP_TICKS; buf[1] = 54; buf[2] = 0; buf[3] = 0;
        buf[4] = CMD_END_BUF;    buf[5] = 0;  buf[6] = 0; buf[7] = 0;
        return;
    }

    budget = VGM_FILL_CMD_BUDGET;
    off = 0;

    /* ── Макрос: проверка границы страницы после rp++ ──────────────
     * При inc HL через #FFFF -> #0000 переключаем VPL-страницу.
     * Fast-path (99.99 %): одна проверка HL==0, ~15 T-states.        */
#define PAGE_CHK() do { \
    if (!(uint16_t)(rp)) { cpg++; wc_mngcvpl(cpg); rp = (uint8_t *)0xC000; } \
} while(0)

    while (off < (CMD_BUF_SIZE - 48))
    {
        /* ── Конец данных? ─────────────────────────────────────── */
        if (cpg > epg || (cpg == epg && (uint16_t)rp >= ead))
        {
            vgm_song_ended = 1;
            break;
        }

        /* ── Читаем опкод ──────────────────────────────────────── */
        op = *rp++; PAGE_CHK();

        /* ═══════ OPL2 write (0x5A) — самая частая ═══════ */
        if (op == 0x5A)
        {
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
            buf[off] = CMD_WRITE_B0;
            buf[off + 1] = b1; buf[off + 2] = b2; buf[off + 3] = 0;
            off += 4;
            goto do_budget;
        }

        /* ═══════ Frame wait 1/60 (0x62) ═══════ */
        if (op == 0x62)
        {
            uint16_t t  = wacc + 735u;
            uint16_t tk = t >> VGM_SAMPLE_SHIFT;
            wacc = t & VGM_SAMPLE_MASK;
            if (tk) {
                emit_wait(buf, &off, tk);
                budget = VGM_FILL_CMD_BUDGET;
            }
            continue;
        }

        /* ═══════ Frame wait 1/50 (0x63) ═══════ */
        if (op == 0x63)
        {
            uint16_t t  = wacc + 882u;
            uint16_t tk = t >> VGM_SAMPLE_SHIFT;
            wacc = t & VGM_SAMPLE_MASK;
            if (tk) {
                emit_wait(buf, &off, tk);
                budget = VGM_FILL_CMD_BUDGET;
            }
            continue;
        }

        /* ═══════ Short wait 1-16 samples (0x70-0x7F) ═══════ */
        if ((op & 0xF0) == 0x70)
        {
            uint16_t t  = wacc + (uint16_t)((op & 0x0Fu) + 1u);
            uint16_t tk = t >> VGM_SAMPLE_SHIFT;
            wacc = t & VGM_SAMPLE_MASK;
            if (tk) {
                emit_wait(buf, &off, tk);
                budget = VGM_FILL_CMD_BUDGET;
            }
            continue;
        }

        /* ═══════ Arbitrary wait (0x61) — uint32 only here ═══════ */
        if (op == 0x61)
        {
            uint32_t t32;
            uint16_t tk;
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
            t32 = (uint32_t)wacc + ((uint16_t)b1 | ((uint16_t)b2 << 8));
            tk  = (uint16_t)(t32 >> VGM_SAMPLE_SHIFT);
            wacc = (uint16_t)(t32 & (uint32_t)VGM_SAMPLE_MASK);
            if (tk) {
                emit_wait(buf, &off, tk);
                budget = VGM_FILL_CMD_BUDGET;
            }
            continue;
        }

        /* ═══════ OPL3 Bank 0 (0x5E) ═══════ */
        if (op == 0x5E)
        {
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
            buf[off] = CMD_WRITE_B0;
            buf[off + 1] = b1; buf[off + 2] = b2; buf[off + 3] = 0;
            off += 4;
            goto do_budget;
        }

        /* ═══════ OPL1 (0x5B) ═══════ */
        if (op == 0x5B)
        {
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
            buf[off] = CMD_WRITE_B0;
            buf[off + 1] = b1; buf[off + 2] = b2; buf[off + 3] = 0;
            off += 4;
            goto do_budget;
        }

        /* ═══════ Y8950 / MSX-AUDIO (0x5C) ═══════ */
        if (op == 0x5C)
        {
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
            buf[off] = CMD_WRITE_B0;
            buf[off + 1] = b1; buf[off + 2] = b2; buf[off + 3] = 0;
            off += 4;
            goto do_budget;
        }

        /* ═══════ OPL3 Bank 1 (0x5F) ═══════ */
        if (op == 0x5F)
        {
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
            buf[off] = CMD_WRITE_B1;
            buf[off + 1] = b1; buf[off + 2] = b2; buf[off + 3] = 0;
            off += 4;
            goto do_budget;
        }

        /* ═══════ YM2203 SSG+FM (0x55) ═══════ */
        if (op == 0x55)
        {
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
#ifdef VGM_FREQ_SCALE
          if (vgm_freq_mode != FREQ_MODE_NATIVE) {
            /* ── PSG tone period (reg 0x00-0x05): 12-bit, парами lo/hi ── */
            if (b1 <= 0x05) {
                uint8_t ch = b1 >> 1;           /* 0,1,2 */
                if (b1 & 1) {
                    /* hi-nibble → обновить shadow, пересчитать полные 12 бит */
                    psg_shadow[ch] = (psg_shadow[ch] & 0x00FF)
                                   | ((uint16_t)(b2 & 0x0F) << 8);
                } else {
                    /* lo-byte → обновить shadow */
                    psg_shadow[ch] = (psg_shadow[ch] & 0x0F00) | b2;
                }
                {
                    uint16_t scaled = freq_lut_base[psg_shadow[ch]];
                    /* Emit lo byte (reg 0,2,4) */
                    buf[off] = CMD_WRITE_AY;
                    buf[off + 1] = ch << 1;  /* reg = 0,2,4 */
                    buf[off + 2] = (uint8_t)(scaled & 0xFF);
                    buf[off + 3] = 0;
                    off += 4;
                    /* Emit hi nibble (reg 1,3,5) */
                    buf[off] = CMD_WRITE_AY;
                    buf[off + 1] = (ch << 1) | 1;  /* reg = 1,3,5 */
                    buf[off + 2] = (uint8_t)((scaled >> 8) & 0x0F);
                    buf[off + 3] = 0;
                    off += 4;
                    goto do_budget;
                }
            }
            /* ── Noise period (reg 0x06): 5-bit ── */
            else if (b1 == 0x06) {
                b2 = (uint8_t)(freq_lut_base[b2 & 0x1F] & 0x1F);
            }
            /* ── FM F-Number hi+Block (reg 0xA4-0xA6): буферизовать ── */
            else if (b1 >= 0xA4 && b1 <= 0xA6) {
                fm_fnum_hi[b1 - 0xA4] = b2;
                /* НЕ записываем в буфер — дождёмся защёлки 0xA0-0xA2 */
                goto do_budget;
            }
            /* ── FM F-Number lo (reg 0xA0-0xA2): защёлка → пересчёт ── */
            else if (b1 >= 0xA0 && b1 <= 0xA2) {
                uint8_t ch = b1 - 0xA0;
                uint8_t hi = fm_fnum_hi[ch];
                uint16_t fnum = ((uint16_t)(hi & 0x07) << 8) | b2;
                uint8_t block = (hi >> 3) & 0x07;
                fnum = freq_lut_base[fnum]; /* LUT: 0..2047 */
                /* Overflow: если fnum > 2047 → увеличить block */
                while (fnum > 2047 && block < 7) {
                    fnum >>= 1;
                    block++;
                }
                if (fnum > 2047) fnum = 2047;
                /* Emit hi byte (0xA4+ch) */
                buf[off] = CMD_WRITE_AY;
                buf[off + 1] = 0xA4 + ch;
                buf[off + 2] = (uint8_t)((block << 3) | ((fnum >> 8) & 0x07));
                buf[off + 3] = 0;
                off += 4;
                /* Emit lo byte (0xA0+ch) — защёлка */
                buf[off] = CMD_WRITE_AY;
                buf[off + 1] = b1;
                buf[off + 2] = (uint8_t)(fnum & 0xFF);
                buf[off + 3] = 0;
                off += 4;
                goto do_budget;
            }
          }
#endif /* VGM_FREQ_SCALE */
            buf[off] = CMD_WRITE_AY;
            buf[off + 1] = b1; buf[off + 2] = b2; buf[off + 3] = 0;
            off += 4;
            goto do_budget;
        }

        /* ═══════ AY8910 dual (0xA0) ═══════ */
        if (op == 0xA0)
        {
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
#ifdef VGM_FREQ_SCALE
          if (vgm_freq_mode != FREQ_MODE_NATIVE) {
            uint8_t reg = b1 & 0x7F;
            /* PSG tone period (reg 0-5): полный 12-бит через shadow */
            if (reg <= 0x05) {
                uint8_t ch = reg >> 1;
                if (reg & 1) {
                    psg_shadow[ch] = (psg_shadow[ch] & 0x00FF)
                                   | ((uint16_t)(b2 & 0x0F) << 8);
                } else {
                    psg_shadow[ch] = (psg_shadow[ch] & 0x0F00) | b2;
                }
                {
                    uint16_t scaled = freq_lut_base[psg_shadow[ch]];
                    /* AY (0xA0): emit оба байта сразу */
                    buf[off] = (b1 & 0x80) ? CMD_WRITE_AY2 : CMD_WRITE_AY;
                    buf[off + 1] = ch << 1;  /* lo reg */
                    buf[off + 2] = (uint8_t)(scaled & 0xFF);
                    buf[off + 3] = 0;
                    off += 4;
                    buf[off] = (b1 & 0x80) ? CMD_WRITE_AY2 : CMD_WRITE_AY;
                    buf[off + 1] = (ch << 1) | 1;  /* hi reg */
                    buf[off + 2] = (uint8_t)((scaled >> 8) & 0x0F);
                    buf[off + 3] = 0;
                    off += 4;
                    goto do_budget;
                }
            }
            /* Noise period (reg 6) */
            else if (reg == 0x06) {
                b2 = (uint8_t)(freq_lut_base[b2 & 0x1F] & 0x1F);
            }
          }
#endif
            buf[off]     = (b1 & 0x80) ? CMD_WRITE_AY2 : CMD_WRITE_AY;
            buf[off + 1] = b1 & 0x7F;
            buf[off + 2] = b2;
            buf[off + 3] = 0;
            off += 4;
            goto do_budget;
        }

        /* ═══════ SAA1099 dual (0xBD) ═══════ */
        if (op == 0xBD)
        {
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
            buf[off]     = (b1 & 0x80) ? CMD_WRITE_SAA2 : CMD_WRITE_SAA;
            buf[off + 1] = b1 & 0x7F;
            buf[off + 2] = b2;
            buf[off + 3] = 0;
            off += 4;
            goto do_budget;
        }

        /* ═══════ End of data (0x66) ═══════ */
        if (op == 0x66)
        {
            vgm_song_ended = 1;
            break;
        }

        /* ═══════ Data block (0x67) — rare ═══════ */
        if (op == 0x67)
        {
            uint32_t sz;
            rp++; PAGE_CHK();  /* 0x66 */
            rp++; PAGE_CHK();  /* type */
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
            sz = (uint16_t)b1 | ((uint16_t)b2 << 8);
            b1 = *rp++; PAGE_CHK();
            b2 = *rp++; PAGE_CHK();
            sz |= ((uint32_t)b1 << 16) | ((uint32_t)b2 << 24);
            /* save → call vgm_skip → reload */
            vgm_read_ptr = (uint16_t)rp;
            vgm_cur_page = cpg;
            vgm_skip(sz);
            rp  = (uint8_t *)vgm_read_ptr;
            cpg = vgm_cur_page;
            continue;
        }

        /* ═══════ PCM RAM write (0x68) — rare ═══════ */
        if (op == 0x68)
        {
            vgm_read_ptr = (uint16_t)rp;
            vgm_cur_page = cpg;
            vgm_skip(11);
            rp  = (uint8_t *)vgm_read_ptr;
            cpg = vgm_cur_page;
            continue;
        }

        /* ═══════ DAC Stream (0x90-0x95) — rare ═══════ */
        if (op >= 0x90 && op <= 0x95)
        {
            static const uint8_t dac_len[] = {4, 4, 5, 10, 1, 4};
            uint8_t n = dac_len[op - 0x90];
            while (n--) { rp++; PAGE_CHK(); }
            continue;
        }

        /* ═══════ Generic skip (all other commands) ═══════ */
        {
            uint8_t n = 0;
            if      (op < 0x30)  n = 0;
            else if (op < 0x40)  n = 1;
            else if (op < 0x4F)  n = 2;
            else if (op <= 0x50) n = 1;
            else if (op < 0x60)  n = 2;
            else if (op < 0xA0)  n = 0;   /* 0x80-0x9F: 1-byte cmds */
            else if (op < 0xC0)  n = 2;
            else if (op < 0xE0)  n = 3;
            else                 n = 4;
            while (n--) { rp++; PAGE_CHK(); }
        }
        continue;

    do_budget:
        if (--budget == 0)
        {
            emit_wait(buf, &off, 1);
            budget = VGM_FILL_CMD_BUDGET;
        }
    }

#undef PAGE_CHK

    /* Записать состояние обратно в глобалы */
    vgm_cur_page    = cpg;
    vgm_read_ptr    = (uint16_t)rp;
    vgm_wait_accum  = wacc;

    /* Завершить буфер */
    buf[off] = CMD_END_BUF;
    buf[off + 1] = 0; buf[off + 2] = 0; buf[off + 3] = 0;
}

/* ── GD3 metadata buffers ────────────────────────────────────────── */
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

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
uint16_t vgm_total_seconds;

/* ── High-level command queue globals ──────────────────────────────── */
hl_entry_t vgm_hl_queue[HL_QUEUE_MAX];
uint8_t vgm_hl_len;
uint8_t vgm_hl_pos;
uint8_t vgm_hl_abort_pos;
uint8_t vgm_loop_count;


/* ── Аккумулятор остатка задержки (VGM samples, 0..15) ─────────────── */
static uint16_t vgm_wait_accum = 0;

#ifdef VGM_FREQ_SCALE
/* ── Масштабирование частот PSG/FM (только LUT) ─────────────────────── */
uint8_t vgm_freq_mode;
uint16_t *freq_lut_base;
static uint8_t freq_lut_page;   /* plugin page для Window 0 (LUT)       */

/* Shadow-регистры PSG: полные 12-бит tone period на канал.
 * Обновляются при записи lo/hi, используются для LUT lookup. */
static uint16_t psg_shadow[3];   /* каналы 0,1,2 — chip 1 */
static uint16_t psg_shadow2[3];  /* каналы 0,1,2 — chip 2 (0xA5) */
#endif /* VGM_FREQ_SCALE */

/* ── Бюджеты для вставки служебных ISR-команд ──────────────────────── */
static uint16_t vgm_sec_budget = ISR_FREQ;  /* тиков до CMD_INC_SEC  */

/* ── Static locals for vgm_fill_buffer / emit_wait ─────────────────
 * Eliminates SDCC's IX-frame (32-byte stack + 19T per ld r,-N(ix)).
 * Static access: ld a,(nn) = 13T, ld hl,(nn) = 16T.                   */
static uint8_t *fb_buf;    /* base of current cmd_buf                   */
static uint8_t *fb_wp;     /* write pointer into cmd_buf                */
static uint8_t *fb_rp;     /* VGM read pointer (#C000-#FFFF)           */
static uint8_t  fb_cpg;    /* local copy of vgm_cur_page               */
static uint8_t  fb_budget; /* commands until forced yield               */
static uint16_t fb_wacc;   /* wait accumulator (0..31)                  */
static uint8_t  fb_op;     /* current VGM opcode                       */
static uint8_t  fb_b1;     /* operand byte 1                           */
static uint8_t  fb_b2;     /* operand byte 2                           */
static uint8_t  fb_is_vgm; /* 1 = VGM mega-buffer, 0 = cmdblk          */
static uint16_t fb_t;      /* temp: wait accumulator + samples          */
static uint16_t fb_tk;     /* temp: tick count for emit_wait            */
static uint32_t fb_t32;    /* temp: 32-bit wait / data-block size       */
static uint16_t fb_w;      /* temp: emit_wait split value               */
static uint8_t  fb_n;      /* temp: skip byte count                    */
static hl_entry_t *fb_e;   /* temp: HL queue entry pointer              */
#ifdef VGM_FREQ_SCALE
static uint8_t  fb_ch;     /* temp: PSG/FM channel index               */
static uint16_t fb_scaled; /* temp: scaled frequency value              */
static uint8_t  fb_reg;    /* temp: AY register (0xA0 handler)         */
#endif

/* VGM_FILL_CMD_BUDGET определён в variant_cfg.h (через isr.h) */


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
    /* ── Расчёт масштабов частот PSG ─────────────────────────────────
     * YM2203: psg_khz = полный master clock → ищем в freq_lut_map_ym[]
     * AY8910: psg_khz = clock как есть      → ищем в freq_lut_map_ay[]
     * Ratio одинаковый: HW_AY/AY_src = HW_YM/YM_src, таблицы общие. */
    vgm_freq_mode = FREQ_MODE_NATIVE;
    freq_lut_base = (uint16_t *)0;
    freq_lut_page = 0;
    psg_shadow[0] = 0; psg_shadow[1] = 0; psg_shadow[2] = 0;
    psg_shadow2[0] = 0; psg_shadow2[1] = 0; psg_shadow2[2] = 0;
    for (i = 0; i < vgm_chip_count; i++) {
        uint16_t psg_khz;
        uint16_t hw_khz;
        uint16_t bypass_tol;
        const freq_lut_entry_t *lut_map;
        uint8_t lut_count;

        if (vgm_chip_list[i].id == VGM_OFF_YM2203) {
            psg_khz    = vgm_chip_list[i].clock_khz;  /* полный master clock */
            hw_khz     = FREQ_LUT_HW_YM_KHZ;          /* 3500 */
            bypass_tol = FREQ_LUT_BYPASS_TOL_YM;       /* 70   */
            lut_map    = freq_lut_map_ym;
            lut_count  = FREQ_LUT_YM_COUNT;
        } else if (vgm_chip_list[i].id == VGM_OFF_AY8910) {
            psg_khz    = vgm_chip_list[i].clock_khz;
            hw_khz     = FREQ_LUT_HW_KHZ;              /* 1750 */
            bypass_tol = FREQ_LUT_BYPASS_TOL;           /* 35   */
            lut_map    = freq_lut_map_ay;
            lut_count  = FREQ_LUT_AY_COUNT;
        } else {
            continue;
        }

        /* ── Определяем режим масштабирования ─────────────────────
         * 1) bypass: |psg_khz - HW| ≤ 2% → native
         * 2) LUT table match (±5%) → table mode (Window 0)
         * Нет match → остаётся native (экзотика <1% файлов) */
        {
            uint16_t diff = (psg_khz > hw_khz)
                          ? (psg_khz - hw_khz)
                          : (hw_khz - psg_khz);
            if (diff <= bypass_tol) {
                /* ±2% — native, no scaling needed */
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

    /* ── Total playback duration (seconds) ────────────────────────────
     * total_samples (0x18) = samples in entire file (before loop).
     * loop_samples  (0x20) = samples in one loop iteration.
     * Effective total = total_samples + loop_samples * MAX_LOOP_REWINDS.
     * Divide by 44100 to get seconds.
     */
    {
        uint32_t ts = (uint32_t)base[0x18]
                    | ((uint32_t)base[0x19] << 8)
                    | ((uint32_t)base[0x1A] << 16)
                    | ((uint32_t)base[0x1B] << 24);
        if (vgm_loop_addr) {
            uint32_t ls = (uint32_t)base[0x20]
                        | ((uint32_t)base[0x21] << 8)
                        | ((uint32_t)base[0x22] << 16)
                        | ((uint32_t)base[0x23] << 24);
            ts += ls * MAX_LOOP_REWINDS;
        }
        vgm_total_seconds = (uint16_t)(ts / 44100UL);
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
 * asm_shift_mask — быстрый 16-бит сдвиг вправо на VGM_SAMPLE_SHIFT
 *
 * Читает fb_t,  пишет fb_tk  = fb_t >> VGM_SAMPLE_SHIFT
 *                     fb_wacc = fb_t &  VGM_SAMPLE_MASK
 * Чистый ASM: ~122..146 T  vs  SDCC цикл ~370..514 T (3-4× быстрее).
 * Безопасно для fb_t ≤ 4095 (H ≤ 0x0F → H<<(8-N) вмещается в байт).
 * Clobbers: A, B, H, L.
 * ───────────────────────────────────────────────────────────────────── */
static void asm_shift_mask(void) __naked {
    __asm
    ld  hl, (_fb_t)
    ;; fb_wacc = fb_t & mask
    ld  a, l
#if VGM_SAMPLE_SHIFT == 6
    and a, #0x3F
#elif VGM_SAMPLE_SHIFT == 5
    and a, #0x1F
#elif VGM_SAMPLE_SHIFT == 4
    and a, #0x0F
#endif
    ld  (_fb_wacc), a
    ;; fb_tk = fb_t >> N  via  H<<(8-N) | L>>N
    ld  a, h
    add a, a
    add a, a
#if VGM_SAMPLE_SHIFT < 6
    add a, a
#endif
#if VGM_SAMPLE_SHIFT < 5
    add a, a
#endif
    ld  b, a
    ld  a, l
    rlca
    rlca
#if VGM_SAMPLE_SHIFT < 6
    rlca
#endif
#if VGM_SAMPLE_SHIFT < 5
    rlca
#endif
#if VGM_SAMPLE_SHIFT == 6
    and a, #0x03
#elif VGM_SAMPLE_SHIFT == 5
    and a, #0x07
#elif VGM_SAMPLE_SHIFT == 4
    and a, #0x0F
#endif
    or  a, b
    ld  (_fb_tk), a
    xor a, a
    ld  (_fb_tk+1), a
    ld  (_fb_wacc+1), a
    ret
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_read_byte — read one byte from fb_rp, handle page crossing
 * Returns A = byte.  Clobbers C, HL.
 * Fast-path: 74 T  (+ call 17 + ret 10 = 101 T total)
 * ───────────────────────────────────────────────────────────────────── */
static uint8_t asm_read_byte(void) __naked
{
    __asm
    ld   hl, (_fb_rp)       ;16  load read pointer
    ld   c, (hl)            ; 7  byte → C
    inc  hl                 ; 6
    ld   a, h               ; 4
    or   a, l               ; 4  Z if HL wrapped to 0x0000
    jr   Z, 150$            ; 7  page-cross (rare)
    ld   (_fb_rp), hl       ;16  store updated pointer
    ld   a, c               ; 4  result → A
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
    ld   a, c               ; 4  result → A
    ret                     ;10
    __endasm;
}

/* ─────────────────────────────────────────────────────────────────────
 * asm_read_2bytes — read two consecutive bytes → fb_b1, fb_b2
 * Writes directly to statics.  Handles page-cross per byte.
 * Fast-path: 124 T  (+ call 17 = 141 T total)
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
                fb_wp[0] = CMD_SKIP_TICKS;
                fb_wp[1] = ISR_TICKS_PER_FRAME - 1;
                fb_wp += 4;
            } else if (fb_w >= 1) {
                fb_wp[0] = CMD_SKIP_TICKS;
                fb_wp[1] = (uint8_t)(fb_w - 1);
                fb_wp += 4;
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
        fb_wp[0] = CMD_SKIP_TICKS;
        fb_wp[1] = (uint8_t)(tk - 1);
        fb_wp += 4;
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
#ifdef VGM_FREQ_SCALE
        /* Re-map LUT page: WC API (print_line etc.) may remap Window 0 */
        if (vgm_freq_mode == FREQ_MODE_TABLE)
            wc_mng0_pl(freq_lut_page);
#endif
        fb_is_vgm = 1;
        break;

    case HLCMD_LOOP:
        if (!vgm_rewind_to_loop()) {
            /* Нет loop-точки — пропустить */
            vgm_hl_pos++;
            goto next_hl;
        }
        vgm_loop_count++;
        fb_e->cmd = HLCMD_PLAY;   /* морфируем, чтобы не перематывать снова */
        fb_cpg  = vgm_cur_page;
        fb_rp   = (uint8_t *)vgm_read_ptr;
        fb_wacc = vgm_wait_accum;
        wc_mngcvpl(fb_cpg);
#ifdef VGM_FREQ_SCALE
        if (vgm_freq_mode == FREQ_MODE_TABLE)
            wc_mng0_pl(freq_lut_page);
#endif
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

    /* ═══════════════════════════════════════════════════════════════
     * Горячий цикл — разбор VGM-опкодов → ISR-команды
     * EOF-проверка удалена (гарантирован 0x66-сентинел в данных).
     * ═══════════════════════════════════════════════════════════════ */
    while (fb_wp < fb_buf + (CMD_BUF_SIZE - 48))
    {
        /* ── Читаем опкод ──────────────────────────────────────── */
        fb_op = asm_read_byte();

        /* ═══════ OPL2 write (0x5A) — самая частая ═══════ */
        if (fb_op == 0x5A)
        {
            asm_read_2bytes();
            fb_wp[0] = CMD_WRITE_B0;
            fb_wp[1] = fb_b1; fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ Frame wait 1/60 (0x62) ═══════ */
        if (fb_op == 0x62)
        {
            fb_t = fb_wacc + 735u;
            asm_shift_mask();
            if (fb_tk) {
                if (fb_tk <= ISR_TICKS_PER_FRAME && fb_tk < vgm_sec_budget) {
                    fb_wp[0] = CMD_SKIP_TICKS;
                    fb_wp[1] = (uint8_t)(fb_tk - 1);
                    fb_wp += 4;
                    vgm_sec_budget -= fb_tk;
                } else {
                    emit_wait(fb_tk);
                }
                fb_budget = VGM_FILL_CMD_BUDGET;
            }
            continue;
        }

        /* ═══════ Frame wait 1/50 (0x63) ═══════ */
        if (fb_op == 0x63)
        {
            fb_t = fb_wacc + 882u;
            asm_shift_mask();
            if (fb_tk) {
                if (fb_tk <= ISR_TICKS_PER_FRAME && fb_tk < vgm_sec_budget) {
                    fb_wp[0] = CMD_SKIP_TICKS;
                    fb_wp[1] = (uint8_t)(fb_tk - 1);
                    fb_wp += 4;
                    vgm_sec_budget -= fb_tk;
                } else {
                    emit_wait(fb_tk);
                }
                fb_budget = VGM_FILL_CMD_BUDGET;
            }
            continue;
        }

        /* ═══════ Short wait 1-16 samples (0x70-0x7F) ═══════ */
#ifndef VGM_NO_SHORT_WAITS
        if ((fb_op & 0xF0) == 0x70)
        {
            fb_t = fb_wacc + (uint16_t)((fb_op & 0x0Fu) + 1u);
            asm_shift_mask();
            if (fb_tk) {
                if (fb_tk <= ISR_TICKS_PER_FRAME && fb_tk < vgm_sec_budget) {
                    fb_wp[0] = CMD_SKIP_TICKS;
                    fb_wp[1] = (uint8_t)(fb_tk - 1);
                    fb_wp += 4;
                    vgm_sec_budget -= fb_tk;
                } else {
                    emit_wait(fb_tk);
                }
                fb_budget = VGM_FILL_CMD_BUDGET;
            }
            continue;
        }
#else
        /* VGM_NO_SHORT_WAITS: пропуск коротких пауз 0x70-0x7F */
        if ((fb_op & 0xF0) == 0x70) continue;
#endif

        /* ═══════ Arbitrary wait (0x61) ═══════ */
        if (fb_op == 0x61)
        {
            asm_read_2bytes();
            fb_t32 = (uint32_t)fb_wacc + ((uint16_t)fb_b1 | ((uint16_t)fb_b2 << 8));
            fb_tk  = (uint16_t)(fb_t32 >> VGM_SAMPLE_SHIFT);
            fb_wacc = (uint16_t)(fb_t32 & (uint32_t)VGM_SAMPLE_MASK);
            if (fb_tk) {
                if (fb_tk <= ISR_TICKS_PER_FRAME && fb_tk < vgm_sec_budget) {
                    fb_wp[0] = CMD_SKIP_TICKS;
                    fb_wp[1] = (uint8_t)(fb_tk - 1);
                    fb_wp += 4;
                    vgm_sec_budget -= fb_tk;
                } else {
                    emit_wait(fb_tk);
                }
                fb_budget = VGM_FILL_CMD_BUDGET;
            }
            continue;
        }

        /* ═══════ OPL3 Bank 0 (0x5E) ═══════ */
        if (fb_op == 0x5E)
        {
            asm_read_2bytes();
            fb_wp[0] = CMD_WRITE_B0;
            fb_wp[1] = fb_b1; fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ OPL1 (0x5B) ═══════ */
        if (fb_op == 0x5B)
        {
            asm_read_2bytes();
            fb_wp[0] = CMD_WRITE_B0;
            fb_wp[1] = fb_b1; fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ Y8950 / MSX-AUDIO (0x5C) ═══════ */
        if (fb_op == 0x5C)
        {
            asm_read_2bytes();
            fb_wp[0] = CMD_WRITE_B0;
            fb_wp[1] = fb_b1; fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ OPL3 Bank 1 (0x5F) ═══════ */
        if (fb_op == 0x5F)
        {
            asm_read_2bytes();
            fb_wp[0] = CMD_WRITE_B1;
            fb_wp[1] = fb_b1; fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ YM2203 SSG+FM (0x55) ═══════ */
        if (fb_op == 0x55)
        {
            asm_read_2bytes();
#ifdef VGM_FREQ_SCALE
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
                /* Emit оба lo+hi: scaled меняет оба байта одновременно */
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
            /* ── Noise period (reg 0x06): 5-bit ── */
            else if (fb_b1 == 0x06) {
                fb_b2 = (uint8_t)(freq_lut_base[fb_b2 & 0x1F] & 0x1F);
            }
            /* FM regs (>0x0D): без масштабирования (TODO: F-Number shadow) */
          }
#endif /* VGM_FREQ_SCALE */
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
#ifdef VGM_FREQ_SCALE
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
                fb_b2 = (uint8_t)(freq_lut_base[fb_b2 & 0x1F] & 0x1F);
            }
          }
#endif /* VGM_FREQ_SCALE */
            fb_wp[0] = CMD_WRITE_AY2;
            fb_wp[1] = fb_b1; fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ AY8910 dual (0xA0) ═══════ */
        if (fb_op == 0xA0)
        {
            asm_read_2bytes();
#ifdef VGM_FREQ_SCALE
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
                /* AY (0xA0): emit оба байта сразу */
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
                fb_b2 = (uint8_t)(freq_lut_base[fb_b2 & 0x1F] & 0x1F);
            }
          }
#endif
            fb_wp[0] = (fb_b1 & 0x80) ? CMD_WRITE_AY2 : CMD_WRITE_AY;
            fb_wp[1] = fb_b1 & 0x7F;
            fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ SAA1099 dual (0xBD) ═══════ */
        /* MultiSound имеет только один физический SAA1099 —
         * chip 2 (bit7=1) пропускаем, играем только chip 1. */
        if (fb_op == 0xBD)
        {
            asm_read_2bytes();
            if (fb_b1 & 0x80)
                goto do_budget;     /* skip chip 2 */
            fb_wp[0] = CMD_WRITE_SAA;
            fb_wp[1] = fb_b1;
            fb_wp[2] = fb_b2;
            fb_wp += 4;
            goto do_budget;
        }

        /* ═══════ End of data (0x66) ═══════ */
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

        /* ═══════ Data block (0x67) — rare ═══════ */
        if (fb_op == 0x67)
        {
            fb_rp++; PAGE_CHK();  /* 0x66 */
            fb_rp++; PAGE_CHK();  /* type */
            asm_read_2bytes();
            fb_t32 = (uint16_t)fb_b1 | ((uint16_t)fb_b2 << 8);
            asm_read_2bytes();
            fb_t32 |= ((uint32_t)fb_b1 << 16) | ((uint32_t)fb_b2 << 24);
            /* save → call vgm_skip → reload */
            vgm_read_ptr = (uint16_t)fb_rp;
            vgm_cur_page = fb_cpg;
            vgm_skip(fb_t32);
            fb_rp  = (uint8_t *)vgm_read_ptr;
            fb_cpg = vgm_cur_page;
            continue;
        }

        /* ═══════ PCM RAM write (0x68) — rare ═══════ */
        if (fb_op == 0x68)
        {
            vgm_read_ptr = (uint16_t)fb_rp;
            vgm_cur_page = fb_cpg;
            vgm_skip(11);
            fb_rp  = (uint8_t *)vgm_read_ptr;
            fb_cpg = vgm_cur_page;
            continue;
        }

        /* ═══════ DAC Stream (0x90-0x95) — rare ═══════ */
        if (fb_op >= 0x90 && fb_op <= 0x95)
        {
            static const uint8_t dac_len[] = {4, 4, 5, 10, 1, 4};
            fb_n = dac_len[fb_op - 0x90];
            while (fb_n--) { fb_rp++; PAGE_CHK(); }
            continue;
        }

        /* ═══════ Generic skip (all other commands) ═══════ */
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

    do_budget:
        if (--fb_budget == 0)
        {
            if (vgm_sec_budget > 1) {
                fb_wp[0] = CMD_SKIP_TICKS;
                fb_wp[1] = 0;
                fb_wp += 4;
                vgm_sec_budget--;
            } else {
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
    fb_wp[0] = CMD_SKIP_TICKS;
    fb_wp[1] = 0;
    fb_wp += 4;

    fb_wp[0] = CMD_END_BUF;
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

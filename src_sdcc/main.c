/*
 * main.c — VGM Info Viewer Plugin для Wild Commander (SDCC/Z80)
 *           [РЕЖИМ ОТЛАДКИ: плеер не запускается]
 *
 * Загружает VGM, парсит заголовок, показывает метаданные.
 * Управление  ESC              = выход (wc_key_esc)
 *             N                = следующий файл (wc_kbscn)
 *             P                = предыдущий файл (wc_kbscn)
 */

#include "inc/types.h"
#include "inc/tsconf.h"
#include "inc/wc_api.h"
#include "inc/txtlib.h"
#include "inc/vgm.h"
#include "inc/isr.h"
#include "inc/keys.h"
#include "inc/opl3.h"
#include <string.h>

/* ── Бордюр-отладка ─────────────────────────────────────────────────── */
static void border(uint8_t c) { sfr_zx_fe = c; }

/* ── Debug port trace (emulator "Illegal port!" log) ─────────────── */
/* Define DBG_ENABLE to compile debug port output; disabled by default */
/* #define DBG_ENABLE */

#ifdef DBG_ENABLE
#define DBG_STATUS  0xFFAF
#define DBG_DATA1   0xFEAF
#define DBG_DATA2   0xFDAF

static void dbg_trace(uint8_t code) __naked {
    (void)code;
    __asm
        ; A = code (sdcccall)
        ld   bc, #0xFFAF
        out  (c), a
        ret
    __endasm;
}

static void dbg_trace2(uint8_t code, uint8_t d1, uint8_t d2) {
    (void)d1; (void)d2;
    __asm
        ; A=code, stack: d1, d2 (sdcccall: A=first, stack rest)
        ld   bc, #0xFFAF
        out  (c), a
        ; d1 = 2(sp), d2 = 3(sp)  — behind ret addr
        ld   hl, #2
        add  hl, sp
        ld   a, (hl)
        ld   bc, #0xFEAF
        out  (c), a
        inc  hl
        ld   a, (hl)
        ld   bc, #0xFDAF
        out  (c), a
    __endasm;
}
#else
#define dbg_trace(code)            ((void)0)
#define dbg_trace2(code, d1, d2)   ((void)0)
#endif
/* ── SAA1099 clock (MultiSound) ──────────────────────────────── */
/* MultiSound перехватывает выбор регистров 0xF0-0xFF в YM2149.
 * Младшие 4 бита номера регистра (top.v):
 *   bit 0 — выбор чипа YM (0=YM1, 1=YM2)
 *   bit 1 — 0=чтение статуса YM2203, 1=normal
 *   bit 2 — 0=FM ON (Z/pullup), 1=FM OFF
 *   bit 3 — 0=SAA1099 clock ON,  1=SAA1099 clock OFF
 */
static void saa_clock_on(void) {
    __asm
        ld   bc, #0xFFFD
        ld   a, #0xF3        ; 0xF3: bit0=1(YM2), bit1=1(normal), bit2=0(FM ON), bit3=0(SAA ON)
        out  (c), a
    __endasm;
}

static void saa_clock_off(void) {
    __asm
        ld   bc, #0xFFFD
        ld   a, #0xFF        ; 0xFF: bit2=1(FM OFF), bit3=1(SAA OFF)
        out  (c), a
    __endasm;
}
/* ── Тишина чипов ────────────────────────────────────────────────────── */
/* Обход vgm_chip_list[] — гасим все обнаруженные чипы при stop_playback.
 * ay_silence / ay2_silence / saa_silence / saa2_silence — в asm/opl3.s  */

static void silence_chips(void)
{
    for (uint8_t i = 0; i < vgm_chip_count; i++) {
        uint8_t id = vgm_chip_list[i].id;
        uint8_t dual = vgm_chip_list[i].flags & VGM_CF_DUAL;

        switch (id) {
        case VGM_OFF_AY8910:
        case VGM_OFF_YM2203:
            ay_silence();
            if (dual) ay2_silence();
            break;

        case VGM_OFF_YM3526:
        case VGM_OFF_YM3812:
        case VGM_OFF_Y8950:
        case VGM_OFF_YMF262:
        case VGM_OFF_YMF278B:
            opl3_reset();
            break;

        case VGM_OFF_SAA1099:
            saa_silence();
            if (dual) saa2_silence();
            break;

        default:
            break;
        }
    }
}

/* ── OPL3 operator offset table ─────────────────────────────────── */
/* OPL3 operators are not contiguous: 0x00-0x05, 0x08-0x0D, 0x10-0x15 */
static const uint8_t opl3_op_offsets[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15
};
#define OPL3_NUM_OPS 18

/* ── Фейд-параметры ─────────────────────────────────────────────────── */
/* FADE_STEPS / FADE_INTERVAL — для ручного выхода (Q/ESC).
 * AUTOFADE — для автоматического затухания перед концом трека.         */
#define FADE_STEPS    16
#define FADE_INTERVAL 1     /* кадров между шагами (1 × 20ms = 20ms) */

static void wait_frames(uint8_t n)
{
    while (n--) {
        uint8_t t0 = isr_tick_ctr;
        while ((uint8_t)(isr_tick_ctr - t0) < ISR_TICKS_PER_FRAME)
            ;
    }
}

/* ── Кешированные флаги активных чипов (для фейда) ────────────────── */
static uint8_t s_has_opl, s_has_ay, s_has_ay2;
static uint8_t s_has_saa, s_has_saa2;

static void detect_active_chips(void)
{
    s_has_opl = s_has_ay = s_has_ay2 = 0;
    s_has_saa = s_has_saa2 = 0;
    for (uint8_t i = 0; i < vgm_chip_count; i++) {
        uint8_t id = vgm_chip_list[i].id;
        uint8_t dual = vgm_chip_list[i].flags & VGM_CF_DUAL;
        switch (id) {
        case VGM_OFF_AY8910:
        case VGM_OFF_YM2203:
            s_has_ay = 1;
            if (dual) s_has_ay2 = 1;
            break;
        case VGM_OFF_YM3526:
        case VGM_OFF_YM3812:
        case VGM_OFF_Y8950:
        case VGM_OFF_YMF262:
        case VGM_OFF_YMF278B:
            s_has_opl = 1;
            break;
        case VGM_OFF_SAA1099:
            s_has_saa = 1;
            if (dual) s_has_saa2 = 1;
            break;
        default:
            break;
        }
    }
}

/* Применить один шаг затухания (0..15) ко всем активным чипам.
 * Работает при включённом ISR — не останавливает воспроизведение. */
static void apply_fade_step(uint8_t step)
{
    if (s_has_opl) {
        uint8_t tl = (uint8_t)(step << 2);  /* 0,4,8,...60 */
        if (tl > 0x3F) tl = 0x3F;
        for (uint8_t op = 0; op < OPL3_NUM_OPS; op++) {
            uint8_t reg = 0x40 + opl3_op_offsets[op];
            opl3_write_b0(reg, tl);
            opl3_write_b1(reg, tl);
        }
    }
    if (s_has_ay) {
        uint8_t vol = step >= 15 ? 0 : (uint8_t)(15 - step);
        ay_write_reg(8, vol);
        ay_write_reg(9, vol);
        ay_write_reg(10, vol);
    }
    if (s_has_ay2) {
        uint8_t vol = step >= 15 ? 0 : (uint8_t)(15 - step);
        ay2_write_reg(8, vol);
        ay2_write_reg(9, vol);
        ay2_write_reg(10, vol);
    }
    if (s_has_saa) {
        uint8_t amp = step >= 15 ? 0 : (uint8_t)((15 - step) * 0x11);
        for (uint8_t r = 0; r < 6; r++)
            saa_write_reg(r, amp);
    }
    if (s_has_saa2) {
        uint8_t amp = step >= 15 ? 0 : (uint8_t)((15 - step) * 0x11);
        for (uint8_t r = 0; r < 6; r++)
            saa2_write_reg(r, amp);
    }
}

/* Быстрый фейд для ручного выхода (Q/ESC): останавливает ISR,
 * гасит за 16 × 20ms = 0.32 сек + 0.5 сек тишина */
static void fade_out_chips(void)
{
    isr_enabled = 0;
    for (uint8_t step = 0; step < FADE_STEPS; step++) {
        apply_fade_step(step);
        wait_frames(FADE_INTERVAL);
    }
    silence_chips();
    wait_frames(25);
}

/* ── Автофейд перед концом трека ─────────────────────────────────────
 * Вычисляем полную длительность: total_samples + max_loops * loop_samples.
 * Начинаем фейд за 1 секунду до конца (ISR продолжает играть).
 * 16 шагов по 2 кадра = 32 кадра = 0.64 сек плавного затухания.      */
static uint16_t s_total_play_sec;
static uint16_t s_fade_start_sec;
static uint8_t  s_auto_fading;
static uint8_t  s_auto_fade_step;
static uint8_t  s_auto_fade_frame;
static uint8_t  s_auto_fade_done;

#define MAX_LOOP_REWINDS 2

static void compute_play_duration(void)
{
    uint32_t total_samples, loop_samples;
    uint16_t total_sec, loop_sec;

    /* Читаем из VGM-заголовка (страница 0 должна быть доступна) */
    wc_mngcvpl(0);
    {
        volatile uint8_t *b = (volatile uint8_t *)0xC000;
        total_samples = (uint32_t)b[0x18]
                      | ((uint32_t)b[0x19] << 8)
                      | ((uint32_t)b[0x1A] << 16)
                      | ((uint32_t)b[0x1B] << 24);
        loop_samples  = (uint32_t)b[0x20]
                      | ((uint32_t)b[0x21] << 8)
                      | ((uint32_t)b[0x22] << 16)
                      | ((uint32_t)b[0x23] << 24);
    }
    wc_mngcvpl(vgm_cur_page);

    total_sec = (uint16_t)(total_samples / 44100UL);
    loop_sec  = (uint16_t)(loop_samples  / 44100UL);

    if (vgm_loop_addr && loop_sec) {
        s_total_play_sec = total_sec + MAX_LOOP_REWINDS * loop_sec;
    } else {
        s_total_play_sec = total_sec;
    }

    /* Начать фейд за 1 секунду до конца */
    s_fade_start_sec = (s_total_play_sec > 1) ? (s_total_play_sec - 1) : 0;
    s_auto_fading = 0;
    s_auto_fade_step = 0;
    s_auto_fade_frame = 0;
    s_auto_fade_done = 0;
}

/* Вызывается каждый кадр (50 Гц) из главного цикла */
static void update_auto_fade(void)
{
    if (s_auto_fade_done || !s_fade_start_sec)
        return;

    if (!s_auto_fading) {
        if (isr_play_seconds >= s_fade_start_sec)
            s_auto_fading = 1;
        else
            return;
    }

    if (s_auto_fade_step >= FADE_STEPS) {
        s_auto_fade_done = 1;
        return;
    }

    /* 2 кадра на шаг → 32 кадра = 0.64 сек плавного затухания */
    s_auto_fade_frame++;
    if (s_auto_fade_frame >= 2) {
        s_auto_fade_frame = 0;
        apply_fade_step(s_auto_fade_step);
        s_auto_fade_step++;
    }
}

/* ── Размеры и цвета окна ───────────────────────────────────────────── */
/* TextMode=2 (wc.ini) → 90×36. Центровка: (90-60)/2=15, (36-20)/2=8   */
#define WND_X 14
#define WND_Y 6
#define WND_W 64
#define WND_H 24

#define CLR_WIN WC_COLOR(WC_WHITE, WC_BLACK)

static wc_window_t s_wnd;
static uint8_t s_pages;
static uint8_t pbinfo_start_row;

static uint8_t s_playback_inited = 0;
static uint8_t s_last_active_buf = 0;
static uint8_t s_buf_ready[2] = {0, 0};
static uint8_t s_loop_count = 0;
static uint8_t s_last_loop_count = 0;
static uint16_t s_last_displayed_sec = 0xFFFF;  /* форсить первую отрисовку */

/* VGZ info for display */
static uint8_t  s_is_vgz;
static uint32_t s_vgm_unpacked_size;
static uint8_t  s_vgz_compressed_pages;

static void ints_disable(void)
{
    __asm di
        __endasm;
}

static void ints_enable(void)
{
    __asm ei
        __endasm;
}

/* Формат MM:SS */
static void append_time_mmss(char *dst, uint16_t sec)
{
    uint16_t min = sec / 60U;
    uint8_t s = (uint8_t)(sec % 60U);

    buf_append_u16_dec(dst, min);
    buf_append_char(dst, ':');
    buf_append_char(dst, (char)('0' + (s / 10U)));
    buf_append_char(dst, (char)('0' + (s % 10U)));
}

/* ── Имя OPL-типа ────────────────────────────────────────────────────── */
static const char *opl_name(void)
{
    if (vgm_chip_type == VGM_CHIP_OPL)
        return "OPL1  YM3526";
    if (vgm_chip_type == VGM_CHIP_OPL2)
        return "OPL2  YM3812";
    if (vgm_chip_type == VGM_CHIP_OPL3)
        return "OPL3  YMF262";
    return "---  (no OPL)";
}

char work_buf[96 + 2];
char error_buf[96 + 2];

enum state_t
{
    STATE_INIT,
    STATE_LOADING,
    STATE_ERROR,
    STATE_PARSE_HEADER,
    STATE_PLAYBACK
} state;

/* ── Предварительный вывод информации (до загрузки/распаковки) ──────── */
static void draw_pre_load_info(void)
{
    uint8_t row = 1;

    buf_clear(work_buf);
    buf_append_str(work_buf, "                   VGM Player ver 2.0a");
    print_line(&s_wnd, row, work_buf, WC_COLOR(WC_BLUE, WC_YELLOW));
    row = 3;

    buf_clear(work_buf);
    buf_append_str(work_buf, "File name   : ");
    buf_append_str(work_buf, wc_file_name);
    print_line(&s_wnd, row++, work_buf, CLR_WIN);

    buf_clear(work_buf);
    buf_append_str(work_buf, "File size   : ");
    buf_append_u32_dec(work_buf, wc_file_size);
    print_line(&s_wnd, row++, work_buf, CLR_WIN);

    buf_clear(work_buf);
    buf_append_str(work_buf, "16Kb pages  : ");
    buf_append_u16_dec(work_buf, (uint16_t)((wc_file_size + 16383U) >> 14));
    print_line(&s_wnd, row, work_buf, CLR_WIN);
}

uint8_t drow_ui(void)
{
    uint8_t row = 1;

    // Заголовок ---------------------------------------------------------------------------------------
    buf_append_str(work_buf, "       VGM Player ver 0.2 (alpha)");
    print_line(&s_wnd, row, work_buf, WC_COLOR(WC_BLUE, WC_YELLOW));
    row += 2;

    // Инструкция --------------------------------------------------------------------------------------
    buf_clear(work_buf);
    buf_append_str(work_buf, "[N]ext [P]rev [Q/ESC] Exit");
    print_line(&s_wnd, 22, work_buf, WC_COLOR(WC_WHITE, WC_BLUE));

    // Данные файла ------------------------------------------------------------------------------------
    buf_clear(work_buf);
    buf_append_str(work_buf, "File name   : ");
    buf_append_str(work_buf, wc_file_name);
    print_line(&s_wnd, row++, work_buf, CLR_WIN);

    buf_clear(work_buf);
    buf_append_str(work_buf, "File size   : ");
    buf_append_u32_dec(work_buf, wc_file_size);
    if (s_is_vgz) {
        buf_append_str(work_buf, " / ");
        buf_append_u32_dec(work_buf, s_vgm_unpacked_size);
    }
    print_line(&s_wnd, row++, work_buf, CLR_WIN);

    buf_clear(work_buf);
    buf_append_str(work_buf, "16Kb pages  : ");
    if (s_is_vgz) {
        buf_append_u16_dec(work_buf, s_vgz_compressed_pages);
        buf_append_str(work_buf, " / ");
    }
    buf_append_u16_dec(work_buf, s_pages);
    print_line(&s_wnd, row, work_buf, CLR_WIN);
    row += 2;

    buf_clear(work_buf);
    buf_append_str(work_buf, "VGM version : ");
    buf_append_u8_hex(work_buf, vgm_version >> 8);
    buf_append_char(work_buf, '.');
    buf_append_u8_hex(work_buf, vgm_version & 0xFF);
    print_line(&s_wnd, row++, work_buf, CLR_WIN);

    for (uint8_t i = 0; i < vgm_chip_count; i++)
    {
        const vgm_chip_entry_t *chip = &vgm_chip_list[i];

        buf_clear(work_buf);
        buf_append_str(work_buf, "Chip : ");
        {
            const char *cname = vgm_chip_name(chip->id);
            if (cname) {
                buf_append_str(work_buf, cname);
            } else {
                buf_append_str(work_buf, "? (0x");
                buf_append_u8_hex(work_buf, chip->id);
                buf_append_char(work_buf, ')');
            }
        }
        buf_append_str(work_buf, " @ ");
        buf_append_u16_dec(work_buf, chip->clock_khz);
        buf_append_str(work_buf, " kHz");
        if (chip->flags & VGM_CF_DUAL)
            buf_append_str(work_buf, " x2");

        print_line(&s_wnd, row++, work_buf, CLR_WIN);
    }

    // loop info
    buf_clear(work_buf);
    buf_append_str(work_buf, "Loop : ");
    if (vgm_loop_addr)
        buf_append_str(work_buf, "Yes");
    else
        buf_append_str(work_buf, "No");
    print_line(&s_wnd, row++, work_buf, CLR_WIN);

#ifdef VGM_FREQ_SCALE
    /* Информация о режиме частот */
    {
        buf_clear(work_buf);
        buf_append_str(work_buf, "Freq : ");
        if (vgm_freq_mode == FREQ_MODE_TABLE) {
            buf_append_str(work_buf, "table");
        } else {
            buf_append_str(work_buf, "native");
        }
        print_line(&s_wnd, row++, work_buf,
            (vgm_freq_mode == FREQ_MODE_NATIVE)
                ? CLR_WIN
                : WC_COLOR(WC_WHITE, WC_GREEN));
    }
#endif

    row++;

    /* ── GD3 метаданные (English) ──────────────────────────────────── */
    if (vgm_gd3_track[0]) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "Title  : ");
        buf_append_str(work_buf, vgm_gd3_track);
        print_line(&s_wnd, row++, work_buf, WC_COLOR(WC_YELLOW, WC_BLACK));
    }
    if (vgm_gd3_game[0]) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "Game   : ");
        buf_append_str(work_buf, vgm_gd3_game);
        print_line(&s_wnd, row++, work_buf, WC_COLOR(WC_YELLOW, WC_BLACK));
    }
    if (vgm_gd3_author[0]) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "Author : ");
        buf_append_str(work_buf, vgm_gd3_author);
        print_line(&s_wnd, row++, work_buf, WC_COLOR(WC_YELLOW, WC_BLACK));
    }
    if (vgm_gd3_system[0]) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "System : ");
        buf_append_str(work_buf, vgm_gd3_system);
        print_line(&s_wnd, row++, work_buf, WC_COLOR(WC_YELLOW, WC_BLACK));
    }

    row++;
    return row;
}

void render_static_info(void)
{
    buf_clear(work_buf);
    buf_append_str(work_buf, "VGM version: ");
    buf_append_u32_dec(work_buf, vgm_version);
    print_line(&s_wnd, 5, work_buf, CLR_WIN);
}

/* Проверить расширение файла: .vgz / .VGZ */
static uint8_t is_vgz_filename(void)
{
    const char *p = wc_file_name;
    const char *dot = 0;
    while (*p) {
        if (*p == '.') dot = p;
        p++;
    }
    if (!dot) return 0;
    dot++;
    if ((dot[0]=='v'||dot[0]=='V') &&
        (dot[1]=='g'||dot[1]=='G') &&
        (dot[2]=='z'||dot[2]=='Z') &&
        dot[3]==0)
        return 1;
    return 0;
}

uint8_t load_vgm(void)
{
    uint8_t i;
    uint32_t fsize = wc_file_size;

    /* Минимальный размер — хотя бы 256 байт */
    if (fsize < 256UL)
        return 0;

    s_pages = (uint8_t)((fsize + 16383UL) >> 14);
    if (!s_pages)
        s_pages = 1;

    /* vgm_end_page / vgm_end_addr устанавливаются в vgm_parse_header()
       из поля EOF offset заголовка VGM — здесь задавать не нужно. */

    for (i = 0; i < s_pages; i++)
    {
        uint8_t rc;
        wc_mngcvpl(i);
        rc = wc_load512(0xC000, 32);

        /* DBG: 0xB0 = loaded page i, rc */
        dbg_trace2(0xB0, i, rc);

        /* WC_EOF нормален на последнем секторе */
        if (rc && rc != WC_EOF)
            return 0;
    }

    wc_mngcvpl(0); /* страница 0 для проверки заголовка */

    /* ── VGZ: gzip magic 0x1F 0x8B ──────────────────────────────── */
    /* ВАЖНО: между wc_mngcvpl и inflate_vgz НЕ ДОЛЖНО быть WC API   *
     * вызовов (print_line и др.), иначе WC сбрасывает Win3 mapping    *
     * и inflate может упасть.                                        */
    if (*(volatile uint8_t *)0xC000 == 0x1F &&
        *(volatile uint8_t *)0xC001 == 0x8B) {

        uint8_t vgz_pages = s_pages;
        uint32_t vgm_size;
        uint8_t ifl_rc;

        s_is_vgz = 1;
        s_vgz_compressed_pages = s_pages;

        /* DBG: 0xA0 = VGZ detected, pages count */
        dbg_trace2(0xA0, vgz_pages, 0);

        if (vgz_pages > 31)    /* TAP: макс. 31 страница */
            return 0;

        /* DBG: 0xA1 = before copy_pages_to_tap */
        dbg_trace(0xA1);

        /* Копировать VGZ из мегабуфера → TAP-страницы (DI/EI внутри) */
        copy_pages_to_tap(vgz_pages);

        /* DBG: 0xA2 = after copy_pages_to_tap, before inflate_vgz */
        dbg_trace2(0xA2, WC_PAGE_TAP_START, WC_PAGE_TVBPG);

        /* Inflate: TAP (#A1+) → мегабуфер (#20+) */
        ifl_rc = inflate_vgz(WC_PAGE_TAP_START, WC_PAGE_TVBPG);

        /* DBG: 0xA3 = after inflate_vgz, result */
        dbg_trace2(0xA3, ifl_rc, 0);

        if (ifl_rc != 0)
            return 0;

        /* DBG: 0xA4 = inflate OK, reading VGM size */
        dbg_trace(0xA4);

        /* Прочитать размер распакованного VGM из заголовка */
        wc_mngcvpl(0);
        {
            /* VGM offset 0x04: EOF offset (отн. 0x04) — LE uint32 */
            uint16_t lo = *(volatile uint16_t *)0xC004;
            uint16_t hi = *(volatile uint16_t *)0xC006;
            vgm_size = ((uint32_t)hi << 16) | lo;
            vgm_size += 4; /* total = eof_offset + 4 */
        }
        s_vgm_unpacked_size = vgm_size;
        s_pages = (uint8_t)((vgm_size + 16383UL) >> 14);
        if (!s_pages)
            s_pages = 1;

        /* DBG: 0xA5 = VGZ done, final pages */
        dbg_trace2(0xA5, s_pages, 0);
    }

    return s_pages;
}

void start_playback(void)
{


    ints_disable();

    vgm_paused = 0;

    /* Первый запуск: инициализируем ISR, сбрасываем счётчик и заранее
       заполняем оба буфера, чтобы ISR не увидел пустой буфер. */
    if (!s_playback_inited)
    {
        s_playback_inited = 1;

        /* Сброс секунд (ISR считает через CMD_INC_SEC) */
        isr_play_seconds = 0;
        s_last_displayed_sec = 0xFFFF;
        s_loop_count = 0;

        /* ISR-состояние */
        isr_active_buf = 0;
        isr_wait_ctr = 0;

        /* Инициализация IM2: перехват вектора WC #5BFF */
        isr_init();

        /* Включить OPL3 mode + wavesel + reset */
        opl3_init();

        /* Включить частоту SAA1099 (если MultiSound) */
        saa_clock_on();

        /* Сброс VGM runtime уже должен быть сделан parse_header().
           Здесь только заливаем первые два буфера. */
        s_buf_ready[0] = 0;
        s_buf_ready[1] = 0;

        vgm_fill_buffer(0);
        s_buf_ready[0] = 1;

        vgm_fill_buffer(1);
        s_buf_ready[1] = 1;

        s_last_active_buf = 0;
    }

    isr_enabled = 1;
    ints_enable();
}

void stop_playback(void)
{
    if (s_playback_inited) {
        if (s_auto_fade_done) {
            /* Автофейд уже завершён — просто останавливаем */
            isr_enabled = 0;
            silence_chips();
        } else {
            /* Ручной выход — быстрый фейд + тишина */
            fade_out_chips();
        }

        ints_disable();
        saa_clock_off();
        isr_deinit();   /* восстановить вектор WC (DI уже сделан) */
    } else {
        ints_disable();
    }

    isr_enabled = 0;
    vgm_paused = 0;

    s_playback_inited = 0;
    s_buf_ready[0] = 0;
    s_buf_ready[1] = 0;
    s_last_active_buf = 0;

    ints_enable();
}

void update_buffer(void)
{
    uint8_t active;
    uint8_t free_idx;

    if (!s_playback_inited)
        return;

    if (!isr_enabled)
        return;

    if (state != STATE_PLAYBACK)
        return;

    active = (uint8_t)(isr_active_buf & 1U);

    /* Если ISR переключился на другой буфер — старый считается освобождённым */
    if (active != s_last_active_buf)
    {
        s_buf_ready[s_last_active_buf] = 0;
        s_last_active_buf = active;
    }

    /* Всегда держим готовым неактивный буфер */
    free_idx = (uint8_t)(active ^ 1U);

    if (!s_buf_ready[free_idx])
    {
        /* Конец трека → перемотка на loop (макс. MAX_LOOP_REWINDS раз) или stop */
        if (vgm_song_ended)
        {
            if (s_loop_count < MAX_LOOP_REWINDS && vgm_rewind_to_loop())
                s_loop_count++;
            else
                return;            /* song finished — silence plays */
        }

        /* DEBUG: red border while fill_buffer works;
           ISR will restore this color on every exit */
        isr_border_color = 2;
        border(2);

        vgm_fill_buffer(free_idx);
        s_buf_ready[free_idx] = 1;

        /* DEBUG: back to black */
        isr_border_color = 0;
        border(0);
    }
}

void update_playback_info(void)
{
    uint16_t sec;

    if (state != STATE_PLAYBACK)
        return;

    sec = isr_play_seconds;

    /* Перерисовывать только при смене секунд или loop —
       сокращает вызовы WC API с 50/сек до 1-2/сек */
    if (sec == s_last_displayed_sec && s_loop_count == s_last_loop_count)
        return;
    s_last_displayed_sec = sec;
    s_last_loop_count = s_loop_count;

    buf_clear(work_buf);
    buf_append_str(work_buf, "Playing  ");

    buf_append_str(work_buf, "Time ");
    append_time_mmss(work_buf, sec);

    /* Всегда показываем номер прохода (1 = первый, 2 = после 1-й перемотки) */
    buf_append_str(work_buf, "  Loop ");
    buf_append_char(work_buf, (char)('1' + s_loop_count));

    print_line(&s_wnd, pbinfo_start_row, work_buf,
               WC_COLOR(WC_GREEN, WC_BLACK));
}

/* ── main() ──────────────────────────────────────────────────────────── */
void main(void)
{
    // Инициализация буферов для строк
    buf_init(work_buf, 96);
    buf_init(error_buf, 96);

    state = STATE_INIT;

    wc_turbopl(2, 0);
    wc_strset((char *)&s_wnd, sizeof(s_wnd), 0);

    // SOW — однолинейная рамка, без буфера фона
    s_wnd.type = WC_WIN_SINGLE;
    s_wnd.cur_mask = 0x07;
    s_wnd.x = WND_X;
    s_wnd.y = WND_Y;
    s_wnd.width = WND_W;
    s_wnd.height = WND_H;
    s_wnd.color = CLR_WIN;
    s_wnd.buf_addr = 0;

    // Восстановить дисплей WC и нарисовать окно
    wc_gedpl();
    wc_prwow(&s_wnd);

    /* Предварительный вывод: имя, размер, страницы (до загрузки) */
    s_is_vgz = 0;
    s_vgm_unpacked_size = 0;
    s_vgz_compressed_pages = 0;
    draw_pre_load_info();

    /* Если файл .vgz — показать "Unpacking..." ДО начала загрузки.     *
     * Это последний WC API вызов перед inflate — после него load_vgm   *
     * работает только через порты, без обращений к WC.                 */
    if (is_vgz_filename()) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "             Unpacking...");
        print_line(&s_wnd, 7, work_buf, WC_COLOR(WC_GREEN, WC_BLACK));
    }

    uint8_t loaded_pages = load_vgm();

    if (!loaded_pages)
    {
        state = STATE_ERROR;
        wc_exit_code = WC_EXIT_ESC;
        buf_append_str(error_buf, "Failed to load VGM file.");
    }
    else
    {
        state = STATE_PLAYBACK;

        if (vgm_parse_header() != VGM_OK)
        {
            state = STATE_ERROR;
            wc_exit_code = WC_EXIT_ESC;
            buf_append_str(error_buf, "Invalid VGM header.");
        }
        else
        {
            /* Парсим GD3 и восстанавливаем VPL-страницу */
            vgm_parse_gd3();
            wc_mngcvpl(vgm_cur_page);

            /* Определяем типы чипов и длительность трека */
            detect_active_chips();
            compute_play_duration();
        }
    }

    pbinfo_start_row = drow_ui();

    if (state == STATE_PLAYBACK)
        start_playback();

    uint8_t row = 1;
    uint8_t key = 0;

    uint8_t tick_prev = isr_tick_ctr;

    /* Главный цикл: isr_tick_ctr меняется ~ISR_FREQ раз/сек,
       опрос клавиатуры и UI-обновление делаем каждые ISR_TICKS_PER_FRAME тиков (~50 Гц) */
    while (state == STATE_PLAYBACK)
    {
        uint8_t tick = isr_tick_ctr;
        uint8_t tick_delta = tick - tick_prev;

        if (tick_delta >= ISR_TICKS_PER_FRAME)
        {
            tick_prev = tick;

            key = read_keys();

            if (key == KEY_ESC)
            {
                stop_playback();
                wc_exit_code = WC_EXIT_ESC;
                break;
            }

            if (key == KEY_NEXT)
            {
                stop_playback();
                wc_exit_code = WC_EXIT_NEXT;
                break;
            }
            if (key == KEY_PREV)
            {
                stop_playback();
                wc_exit_code = WC_EXIT_PREV;
                break;
            }

            update_playback_info();
            update_auto_fade();
        }

        if (state == STATE_PLAYBACK)
        {
            update_buffer();

            /* Трек закончился (loop исчерпан или нет loop) → следующий */
            if (vgm_song_ended)
            {
                stop_playback();
                wc_exit_code = WC_EXIT_NEXT;
                break;
            }
        }
    }

    stop_playback();
    wc_rresb(&s_wnd);
}

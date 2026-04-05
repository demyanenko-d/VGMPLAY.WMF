/*
 * main.c — VGM Player Plugin для Wild Commander (SDCC/Z80)
 *
 * Поддерживаемые форматы:
 *   VGM (Raw)   — OPL1/OPL2/OPL3, AY-3-8910/YM2149/YM2203, SAA1099
 *   VGZ (gzip)  — автораспаковка через inflate
 *
 * Функции:
 *   - Отображение метаданных (GD3: автор, игра, трек, система)
 *   - Воспроизведение с двойной буферизацией (ISR 1367 Гц)
 *   - OPL3: NEW=1 (L/R через C0-C8); OPL1/OPL2: NEW=0 (compat mode)
 *
 * Управление:
 *   Q        — выход
 *   N        — следующий файл в каталоге
 *   P        — предыдущий файл
 */

#include "inc/types.h"
#include "inc/tsconf.h"
#include "inc/wc_api.h"
#include "inc/txtlib.h"
#include "inc/vgm.h"
#include "inc/isr.h"
#include "inc/keys.h"
#include "inc/version.h"
#include <string.h>

/* Stringify helpers for compile-time ISR_FREQ / budget in title */
#define _STR(x) #x
#define STR(x)  _STR(x)

/* ── Inflate progress bar params (defined in inflate_call.s _DATA) ────── */
extern uint8_t  ifl_pb_text_pg;
extern uint16_t ifl_pb_attr_ofs;
extern uint8_t  ifl_pb_total;
extern uint8_t  ifl_pb_bw;
extern uint8_t  ifl_pb_green;

/* ── Бордюр-отладка ─────────────────────────────────────────────────── */
/* Define DEBUG_BORDER to show fill_buffer timing via border color */
/* #define DEBUG_BORDER */

#ifdef DEBUG_BORDER
static void border(uint8_t c) { sfr_zx_fe = c; }
#endif

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

/* ── Кешированные флаги активных чипов ────────────────────────────── */
static uint8_t s_has_opl, s_has_ay, s_has_ay2;
static uint8_t s_has_saa, s_has_saa2;
static uint8_t s_has_ym2203, s_has_ym2203_2;

static void detect_active_chips(void)
{
    s_has_opl = s_has_ay = s_has_ay2 = 0;
    s_has_saa = s_has_saa2 = 0;
    s_has_ym2203 = s_has_ym2203_2 = 0;
    for (uint8_t i = 0; i < vgm_chip_count; i++) {
        uint8_t id = vgm_chip_list[i].id;
        uint8_t dual = vgm_chip_list[i].flags & VGM_CF_DUAL;
        switch (id) {
        case VGM_OFF_YM2203:
            s_has_ym2203 = 1;
            if (dual) s_has_ym2203_2 = 1;
            /* fallthrough */
        case VGM_OFF_AY8910:
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

/* cfg_loop_rewinds, cfg_min_duration, cfg_max_duration are in vgm.c */

/* ── HL queue helpers ────────────────────────────────────────────────
 * The queue itself and its state live in vgm.c (vgm_hl_queue etc.)
 * vgm_fill_buffer() reads them internally; main.c only builds
 * the queue and sets vgm_hl_pos on abort (Q/N/P).               */

static void hl_push(uint8_t cmd, uint8_t param)
{
    if (vgm_hl_len < HL_QUEUE_MAX) {
        vgm_hl_queue[vgm_hl_len].cmd = cmd;
        vgm_hl_queue[vgm_hl_len].param = param;
        vgm_hl_len++;
    }
}

/* ── Build the playback queue based on detected chips / loop info ─── */
static void build_playback_queue(void)
{
    vgm_hl_len = 0;

    /* Init phase: silence (clear residual state) + init */
    if (s_has_opl) {
        hl_push(HLCMD_CMDBLK, CMDBLK_SILENCE_OPL);
        hl_push(HLCMD_CMDBLK,
                (vgm_chip_type == VGM_CHIP_OPL3) ? CMDBLK_INIT_OPL3
                                                  : CMDBLK_INIT_OPL2);
    }

    /* Enable SAA clock via MultiSound ctrl before playback.
     * CMDBLK_SAA_CLK_ON writes 0xF3 to #FFFD through the ISR command
     * queue, ensuring SAA clock persists even if WC handler touches #FFFD. */
    if (s_has_saa)
        hl_push(HLCMD_CMDBLK, CMDBLK_SAA_CLK_ON);

    /* Main playback */
    hl_push(HLCMD_PLAY, 0);

    /* Loop repeats — только если vgm_parse_header разрешил loop.
     * Policy: без full-track loop, без коротких (<10с), без длинных (>4мин). */
    if (vgm_loop_enabled) {
        for (uint8_t i = 0; i < cfg_loop_rewinds; i++)
            hl_push(HLCMD_LOOP, 0);
    }

    /* ── Shutdown sequence (abort target) ──────────────────────── */
    vgm_hl_abort_pos = vgm_hl_len;

    if (s_has_opl)
        hl_push(HLCMD_CMDBLK, CMDBLK_SILENCE_OPL);
    if (s_has_ym2203)
        hl_push(HLCMD_CMDBLK, CMDBLK_SILENCE_YM2203);
    else if (s_has_ay)
        hl_push(HLCMD_CMDBLK, CMDBLK_SILENCE_AY);
    if (s_has_ym2203_2)
        hl_push(HLCMD_CMDBLK, CMDBLK_SILENCE_YM2203_2);
    else if (s_has_ay2)
        hl_push(HLCMD_CMDBLK, CMDBLK_SILENCE_AY2);
    if (s_has_saa)
        hl_push(HLCMD_CMDBLK, CMDBLK_SILENCE_SAA);
    /* SAA2 silence не нужен — MultiSound имеет только один SAA1099 */

    hl_push(HLCMD_ISR_DONE, 0);

    vgm_hl_pos = 0;
}

/* ── Размеры и цвета окна ───────────────────────────────────────────── */
/* Координаты вычисляются динамически в main() через wc_get_height().
 * TextMode=0: 80×25, TextMode=1: 80×30, TextMode=2: 90×36.           */
#define WND_W 62
#define WND_H 22

#define CLR_WIN WC_COLOR(WC_WHITE, WC_BLACK)
#define CLR_TITLE WC_COLOR(WC_BLUE, WC_YELLOW)

static const char s_win_title[] = " VGM Player " APP_VERSION " ";

/* ── Фиксированные строки окна (1-based content rows) ───────────────
 * Height=23 → interior rows 1..21, border rows 0 and 22.
 *   Row 1:      Title (blue)
 *   Rows 2-4:   File info section
 *   Rows 5-6:   (empty)
 *   Row 7:      ← divider 1 (from_bottom = 23-7 = 16)
 *   Rows 8-14:  VGM info section (ver, chips, loop, freq, GD3)
 *   Row 15:     ← divider 2 (from_bottom = 23-15 = 8)
 *   Row 16:     Progress bar
 *   Rows 17-20: Spectrum analyzer (4 rows)
 *   Row 21:     Help keys                                            */
#define ROW_TITLE         1
#define ROW_FILE_START    2
#define ROW_VGM_START     6
#define ROW_VGM_END       13   /* максимальная строка для VGM/GD3 info   */
#define ROW_PROGRESS      15
#define ROW_SPECTRUM      16   /* 4 rows: 16,17,18,19                    */
#define ROW_HELP          20
#define DIV1_FROM_BOTTOM  16   /* row 7 = 23-16                          */
#define DIV2_FROM_BOTTOM  7    /* row 15 = 23-8                          */

static wc_window_t s_wnd;
static uint8_t s_pages;
/* pbinfo_start_row replaced by ROW_PROGRESS constant */

static uint8_t s_playback_inited = 0;
static uint8_t s_last_active_buf = 0;
static uint8_t s_buf_ready[2] = {0, 0};
static uint8_t s_last_loop_count = 0;
static uint16_t s_last_displayed_sec = 0;  /* 0 = совпадает с начальным sec */

/* BCD-цифры времени (инкремент вместо делений) ─────────────────────── */
static uint8_t s_pb_d_min10 = '0';
static uint8_t s_pb_d_min1  = '0';
static uint8_t s_pb_d_sec10 = '0';
static uint8_t s_pb_d_sec1  = '0';

/* ── Progress bar state (предвычисленные параметры) ───────────────── */
uint8_t  s_pb_text_pg;            /* physical page of text screen (non-static: ASM) */
static uint16_t s_pb_char_ofs;    /* char area offset within text page     */
static uint16_t s_pb_attr_ofs;    /* attr area offset within text page     */
static uint8_t  s_pb_width;       /* bar width in columns                  */
static uint16_t s_pb_err;         /* Bresenham error accumulator           */
static uint8_t  s_pb_col;         /* current green column count            */

/* Позиции изменяемых символов внутри строки progress bar:
 * "Playing  MM:SS / MM:SS  Loop N"
 *  0         9       17     27  29  */
#define PB_OFS_MIN10   9
#define PB_OFS_MIN1   10
#define PB_OFS_SEC10  12
#define PB_OFS_SEC1   13
#define PB_OFS_LOOP   29

/* ── Spectrum analyzer rendering (ASM: asm/spectrum.s) ────────────────
 * 16 bars, 4 rows (ROW_SPECTRUM..ROW_SPECTRUM+3), bottom-up.
 * spectrum_render() and hot-loop spectrum helpers are in asm/spectrum.s.
 * Variables below are non-static so ASM can access them via _symbol. */

/* Предвычисленные адреса для прямого доступа к текстовой странице */
uint16_t s_spec_char_ofs[4];  /* char offsets for 4 rows       */
uint16_t s_spec_attr_ofs[4];  /* attr offsets for 4 rows       */

/* spectrum_render/font/decay() are in asm/spectrum.s */
void spectrum_render(void);
void spectrum_decay(void);
void spectrum_decay_reset(void);
void spectrum_font_init(void);
void spectrum_font_restore(void);


/* ── FSM-клавиатура (дебаунс без сжигания CPU) ───────────────────────
 * Состояния:
 *   WAIT_RELEASE — ждём отпускания ВСЕХ клавиш (защита от «залипания»
 *                  при перезапуске плагина с зажатой N/P/Q)
 *   IDLE         — ничего не нажато, ждём нажатия
 *   CONFIRM      — нажатие обнаружено, подтверждаем через
 *                  KBD_DEBOUNCE_TICKS (~3 мс) стабильного удержания
 * После подтверждения → WAIT_RELEASE (нет авто-повтора).             */
#define KBD_ST_WAIT_RELEASE 0
#define KBD_ST_IDLE         1
#define KBD_ST_CONFIRM      2
#define KBD_DEBOUNCE_TICKS  4u  /* ~3 мс при ISR_FREQ=1367             */

static uint8_t kbd_state;
static uint8_t kbd_raw;      /* кандидат-клавиша в CONFIRM             */
static uint8_t kbd_tick;     /* isr_tick_ctr при входе в CONFIRM       */

static void kbd_init(void)
{
    kbd_state = KBD_ST_WAIT_RELEASE;
    kbd_raw   = KEY_NONE;
}

static uint8_t kbd_poll(void)
{
    uint8_t k = read_keys();

    switch (kbd_state) {
    case KBD_ST_WAIT_RELEASE:
        if (k == KEY_NONE)
            kbd_state = KBD_ST_IDLE;
        return KEY_NONE;

    case KBD_ST_IDLE:
        if (k != KEY_NONE) {
            kbd_raw  = k;
            kbd_tick = isr_tick_ctr;
            kbd_state = KBD_ST_CONFIRM;
        }
        return KEY_NONE;

    case KBD_ST_CONFIRM:
        if (k == KEY_NONE) {
            /* Отпущена до подтверждения — глюк/дребезг */
            kbd_state = KBD_ST_IDLE;
            return KEY_NONE;
        }
        if (k != kbd_raw) {
            /* Другая клавиша — начать подтверждение заново */
            kbd_raw  = k;
            kbd_tick = isr_tick_ctr;
            return KEY_NONE;
        }
        /* Та же клавиша — проверяем интервал */
        if ((uint8_t)(isr_tick_ctr - kbd_tick) >= KBD_DEBOUNCE_TICKS) {
            kbd_state = KBD_ST_WAIT_RELEASE;
            return kbd_raw;
        }
        return KEY_NONE;
    }
    return KEY_NONE;
}

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

/* Формат MM:SS в буфер (5 символов, без '\0') */
static void fmt_mmss(char *out, uint16_t sec)
{
    uint16_t min = sec / 60U;
    uint8_t s = (uint8_t)(sec % 60U);
    /* Минуты: макс 2 цифры (до 99) */
    if (min >= 10)
        *out++ = (char)('0' + (uint8_t)(min / 10U));
    else
        *out++ = '0';
    *out++ = (char)('0' + (uint8_t)(min % 10U));
    *out++ = ':';
    *out++ = (char)('0' + (s / 10U));
    *out++ = (char)('0' + (s % 10U));
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
    uint8_t row = ROW_FILE_START;

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
    uint8_t row;
    uint8_t gd3_count = 0;

    /* ── Section 1: File info (rows 1-3) ────────────────────────── */
    row = ROW_FILE_START;

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

    /* ── Section 2: VGM info (rows 5..ROW_VGM_END) ─────────────── */
    row = ROW_VGM_START;

    buf_clear(work_buf);
    buf_append_str(work_buf, "VGM version : ");
    buf_append_u8_hex(work_buf, vgm_version >> 8);
    buf_append_char(work_buf, '.');
    buf_append_u8_hex(work_buf, vgm_version & 0xFF);
    print_line(&s_wnd, row++, work_buf, CLR_WIN);

    for (uint8_t i = 0; i < vgm_chip_count && row <= ROW_VGM_END; i++)
    {
        /* Compact: show max 2 full chip lines; rest as "+ N more" */
        if (i >= 2 && vgm_chip_count > 3) {
            buf_clear(work_buf);
            buf_append_str(work_buf, "              + ");
            buf_append_u16_dec(work_buf, (uint16_t)(vgm_chip_count - 2));
            buf_append_str(work_buf, " more");
            print_line(&s_wnd, row++, work_buf, CLR_WIN);
            break;
        }

        const vgm_chip_entry_t *chip = &vgm_chip_list[i];

        buf_clear(work_buf);
        buf_append_str(work_buf, "Chip        : ");
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

    /* Loop info */
    if (row <= ROW_VGM_END) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "Loop        : ");
        if (vgm_loop_addr)
            buf_append_str(work_buf, "Yes");
        else
            buf_append_str(work_buf, "No");
        print_line(&s_wnd, row++, work_buf, CLR_WIN);
    }

    /* Информация о режиме частот */
    if (row <= ROW_VGM_END) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "Freq        : ");
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

    /* ── GD3 метаданные (English, max 4) ──────────────────────────── */
    if (vgm_gd3_track[0] && row <= ROW_VGM_END && gd3_count < 4) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "Title  : ");
        buf_append_str(work_buf, vgm_gd3_track);
        print_line(&s_wnd, row++, work_buf, WC_COLOR(WC_YELLOW, WC_BLACK));
        gd3_count++;
    }
    if (vgm_gd3_game[0] && row <= ROW_VGM_END && gd3_count < 4) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "Game   : ");
        buf_append_str(work_buf, vgm_gd3_game);
        print_line(&s_wnd, row++, work_buf, WC_COLOR(WC_YELLOW, WC_BLACK));
        gd3_count++;
    }
    if (vgm_gd3_author[0] && row <= ROW_VGM_END && gd3_count < 4) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "Author : ");
        buf_append_str(work_buf, vgm_gd3_author);
        print_line(&s_wnd, row++, work_buf, WC_COLOR(WC_YELLOW, WC_BLACK));
        gd3_count++;
    }
    if (vgm_gd3_system[0] && row <= ROW_VGM_END && gd3_count < 4) {
        buf_clear(work_buf);
        buf_append_str(work_buf, "System : ");
        buf_append_str(work_buf, vgm_gd3_system);
        print_line(&s_wnd, row++, work_buf, WC_COLOR(WC_YELLOW, WC_BLACK));
        gd3_count++;
    }

    /* ── Help line (fixed row) ──────────────────────────────────── */
    buf_clear(work_buf);
    buf_append_str(work_buf, " [N]ext [P]rev [Q] Exit");
    print_line(&s_wnd, ROW_HELP, work_buf, CLR_TITLE);

    return ROW_PROGRESS;
}

/* render_static_info() removed — replaced by drow_ui() */

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

        /* Set progress bar total (estimated output pages) */
        {
            uint8_t est = (uint8_t)(vgz_pages * 3u);
            if (est > 64) est = 64;
            ifl_pb_total = est;
        }

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

    /* Enable 512-byte cache for Window 2 (#8000-#BFFF).
     * Speeds up code fetches and static fb_* variable access.
     * MUST be after inflate/load — DMA/page I/O would corrupt cache.
     * SysConfig bit 2 = global enable, CacheConfig bit 2 = win2.    */
    //tsconf_set_cache(TSCONF_CACHE_EN_8000);
    //tsconf_set_cpu_cache(TSCONF_CPU_14MHZ, 1);

    vgm_paused = 0;

    if (!s_playback_inited)
    {
        s_playback_inited = 1;

        /* Сброс счётчиков */
        isr_play_seconds = 0;
        s_last_displayed_sec = 0;
        s_pb_d_min10 = '0';
        s_pb_d_min1  = '0';
        s_pb_d_sec10 = '0';
        s_pb_d_sec1  = '0';
        vgm_loop_count = 0;

        /* ISR-состояние */
        isr_active_buf = 0;
        isr_wait_ctr = 0;
        isr_done = 0;

        /* MultiSound ctrl: enable FM/SAA only for chips present.
         * 0xF2 = FM ON + SAA ON + normal read + chip1 (base).
         * bit2: 1 = FM OFF  (set when no YM2203)
         * bit3: 1 = SAA OFF (set when no SAA1099) */
        isr_ms_ctrl = 0xF2
            | (s_has_ym2203 ? 0x00 : 0x04)
            | (s_has_saa ? 0x00 : 0x08);

        /* Apply MultiSound config to hardware immediately
         * (needed for SAA-only music where no AY writes occur) */
        __asm
            ld   bc, #0xFFFD
            ld   a, (_isr_ms_ctrl)
            out  (c), a
        __endasm;

        // SAA1099: принудительный Sound Enable ON до начала воспроизведения.

        if (s_has_saa) {
            __asm
                ;; TURBO 7 MHz
                ld   bc, #0x20AF
                ld   a, #0x01       ; TURBO_7MHZ
                out  (c), a
                ;; SAA1: reg 0x1C (Sound Enable)
                ld   a, #0x1C
                ld   bc, #0x00FF    ; SAA1 address port
                out  (c), a
                ;; SAA1: val 0x01 (Sound Enable ON)
                ld   a, #0x01
                ld   b, #0x01      ; BC = #01FF  SAA1 data port
                out  (c), a
                ;; TURBO 14 MHz
                ld   bc, #0x20AF
                ld   a, #0x02       ; TURBO_14MHZ
                out  (c), a
            __endasm;
        }

        /* Build HL command queue for this track */
        build_playback_queue();

        /* Инициализация IM2: перехват вектора WC #5BFF */
        isr_init();

        /* Fill both buffers from the HL queue.
         * vgm_fill_buffer handles HL transitions internally —
         * init cmdblocks + start of VGM data may all go in buf 0. */
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
    if (!s_playback_inited) {
        ints_disable();
        ints_enable();
        return;
    }

    ints_disable();
    isr_enabled = 0;
    isr_deinit();   /* восстановить вектор WC */

    /* MultiSound: disable FM + SAA, select chip1 (hardware + variable) */
    isr_ms_ctrl = 0xFE;
    __asm
        ld   bc, #0xFFFD
        ld   a, #0xFE          ;; chip1 + FM OFF + SAA CLK OFF
        out  (c), a
    __endasm;

    /* Disable cache before returning to WC.
     * CacheConfig = 0 disables all per-window caches.
     * SysConfig: keep 14 MHz, clear global cache bit.  */
    //tsconf_set_cpu_cache(TSCONF_CPU_14MHZ, 0);
    //tsconf_set_cache(TSCONF_CACHE_NONE);
    

    vgm_paused = 0;
    s_playback_inited = 0;
    s_buf_ready[0] = 0;
    s_buf_ready[1] = 0;
    s_last_active_buf = 0;
    isr_done = 0;

    ints_enable();
}

/**
 * Мгновенный abort воспроизведения.
 *
 * При обычном abort через vgm_hl_abort_pos main loop ждёт, пока ISR
 * доиграет активный буфер (включая CMD_WAIT, до ~1 сек задержки).
 *
 * instant_abort() обходит это за ~1.5 мс:
 *   1. isr_enabled = 0 → ISR пропускает команды (pos_table работает).
 *   2. HALT — CPU спит до следующего INT.  После пробуждения ISR
 *      гарантированно отработал один проход с enabled=0.
 *   3. Обнуляем isr_wait_ctr (отменяем CMD_WAIT).
 *   4. Заполняем НЕАКТИВНЫЙ буфер shutdown-цепочкой
 *      (silence + CMD_ISR_DONE) — ISR его не читает.
 *   5. Переключаем isr_active_buf + isr_read_ptr на этот буфер.
 *   6. isr_enabled = 1 → ISR мгновенно выполняет shutdown.
 *
 * Вызывать ПОСЛЕ установки vgm_hl_pos = vgm_hl_abort_pos.
 * После возврата isr_done будет установлен через 1-2 ISR-тика.
 *
 * Нулевой overhead на горячем пути ISR — без дополнительных флагов.
 */
static void instant_abort(void)
{
    uint8_t free_idx;

    /* ── 1. Заморозить ISR (команды не выполняются) ─────────── */
    isr_enabled = 0;

    /* ── 2. HALT: спать до следующего INT ──────────────────── *
     * После HALT ISR гарантированно отработал с enabled=0,     *
     * не тронул ни один буфер.  ~1.5 мс максимум.             */
    __asm__( "halt" );

    /* ── 3. Обнулить wait_ctr (отменить CMD_WAIT) ─────────── */
    isr_wait_ctr = 0;

    /* ── 4. Заполнить НЕАКТИВНЫЙ буфер shutdown-цепочкой ───── *
     * vgm_hl_pos уже установлен на vgm_hl_abort_pos →         *
     * vgm_fill_buffer выдаст silence + CMD_ISR_DONE.           */
    free_idx = (isr_active_buf & 1) ^ 1;
    vgm_fill_buffer(free_idx);

    /* ── 5. Переключить ISR на заполненный буфер ─────────── */
    isr_active_buf = free_idx;
    isr_read_ptr = (uint16_t)(free_idx ? cmd_buf_b : cmd_buf_a);

    /* ── 6. Разморозить → ISR обработает shutdown ───────── */
    isr_enabled = 1;
}

void update_buffer(void)
{
    uint8_t active;
    uint8_t free_idx;

    if (!s_playback_inited || !isr_enabled || isr_done)
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

    if (s_buf_ready[free_idx])
        return;

    /* Fill the free buffer — vgm_fill_buffer handles HL transitions */
#ifdef DEBUG_BORDER
    isr_border_color = 2;
    border(2);
#endif

    vgm_fill_buffer(free_idx);
    s_buf_ready[free_idx] = 1;

#ifdef DEBUG_BORDER
    isr_border_color = 0;
    border(0);
#endif
}

void update_playback_info(void)
{
    uint16_t sec;
    uint8_t sec_changed, loop_changed;

    if (state != STATE_PLAYBACK)
        return;

    sec = isr_play_seconds;

    sec_changed  = (sec != s_last_displayed_sec);
    loop_changed = (vgm_loop_count != s_last_loop_count);

    if (!sec_changed && !loop_changed)
        return;

    /* ── Переключаем Window3 на текстовую страницу напрямую через порт.
     * Не используем wc_mngcvpl — она вызывает WC_ENTRY и обновляет
     * WC_SYS_PGC, что может корраптить состояние WC при перехваченном ISR.
     * sfr_page3 = прямая запись в порт 0x13AF. */
    {
        uint8_t saved_pg = sfr_page3;
        sfr_page3 = s_pb_text_pg;

        volatile uint8_t *base =
            (volatile uint8_t *)(0xC000 + s_pb_char_ofs);

        /* ── Время: инкрементальный BCD-счётчик.
         * Секунды всегда растут на 1 → вместо 6 делений (sec/60,
         * sec%60, min/10, min%10, s/10, s%10) — только INC + CP.
         * ~20 тактов вместо ~6000. */
        if (sec_changed) {
            s_last_displayed_sec = sec;
            if (++s_pb_d_sec1 > '9') {
                s_pb_d_sec1 = '0';
                if (++s_pb_d_sec10 > '5') {
                    s_pb_d_sec10 = '0';
                    if (++s_pb_d_min1 > '9') {
                        s_pb_d_min1 = '0';
                        s_pb_d_min10++;
                    }
                }
            }
            base[PB_OFS_MIN10] = s_pb_d_min10;
            base[PB_OFS_MIN1]  = s_pb_d_min1;
            base[PB_OFS_SEC10] = s_pb_d_sec10;
            base[PB_OFS_SEC1]  = s_pb_d_sec1;
        }

        if (loop_changed) {
            s_last_loop_count = vgm_loop_count;
            base[PB_OFS_LOOP] = '1' + vgm_loop_count;
        }

        /* ── Progress bar: Bresenham-аккумулятор.
         * Каждую секунду добавляем s_pb_width к ошибке,
         * когда ошибка >= total — красим колонку и вычитаем.
         * Ноль делений.  Точно для любого соотношения sec/width. */
        if (sec_changed && vgm_total_seconds) {
            s_pb_err += s_pb_width;
            while (s_pb_col < s_pb_width &&
                   s_pb_err >= vgm_total_seconds) {
                *(volatile uint8_t *)(0xC000 + s_pb_attr_ofs + s_pb_col) =
                    WC_COLOR(WC_GREEN, WC_BLACK);
                s_pb_col++;
                s_pb_err -= vgm_total_seconds;
            }
        }

        sfr_page3 = saved_pg;
    }
}

/* ── main() ──────────────────────────────────────────────────────────── */
void main(void)
{
    // Инициализация буферов для строк
    buf_init(work_buf, 96);
    buf_init(error_buf, 96);

    state = STATE_INIT;

    wc_turbopl(WC_TURBO_CPU, 0x02);  /* 14 МГц */

    /* ── Чтение INI-параметров ───────────────────────────────────────
     * Формат в wc.ini:  VGMPLAY.WMF -min_dur=10 -max_dur=4 -loops=1
     *   param 0: min_duration секунд  (default 10)
     *   param 1: max_duration в минутах (default 4 → 240 сек)
     *   param 2: loop_count           (default 1)
     * wc_prm_pl() возвращает 0xFF если параметр не задан.            */
    {
        uint8_t v;
        v = wc_prm_pl(0);
        if (v != 0xFF) cfg_min_duration = v;

        v = wc_prm_pl(1);
        if (v != 0xFF) cfg_max_duration = (uint16_t)v * 60u;

        v = wc_prm_pl(2);
        if (v != 0xFF) cfg_loop_rewinds = v;
    }

    wc_strset((char *)&s_wnd, sizeof(s_wnd), 0);

    // SOW — рамка с тенью (TYPE3, заголовок рисуем вручную)
    s_wnd.type = WC_WIN_TYPE3 | WC_WIN_SHADOW;
    s_wnd.cur_mask = 0x05;

    /* Dynamic centering based on WC text mode */
    {
        uint8_t scr_h = wc_get_height();   /* 25/30/36 */
        uint8_t scr_w = (scr_h >= 36) ? 90 : 80;
        s_wnd.x = (scr_w > WND_W) ? (scr_w - WND_W) / 2 : 0;
        s_wnd.y = (scr_h > WND_H) ? (scr_h - WND_H) / 2 : 0;
    }

    s_wnd.width = WND_W;
    s_wnd.height = WND_H;
    s_wnd.color = CLR_WIN;
    s_wnd.buf_addr = 0;
    s_wnd.divider1 = DIV1_FROM_BOTTOM;
    s_wnd.divider2 = DIV2_FROM_BOTTOM;

    // Восстановить дисплей WC и нарисовать окно
    wc_gedpl();
    wc_prwow(&s_wnd);

    /* Заголовок окна синим текстом */
    buf_clear(work_buf);
    buf_append_str(work_buf, s_win_title);
    print_line(&s_wnd, ROW_TITLE, work_buf, CLR_TITLE);

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
        print_line(&s_wnd, ROW_VGM_START, work_buf, WC_COLOR(WC_YELLOW, WC_BLACK));

        /* Setup progress bar: get text screen address for VGM section col 2 */
        {
            uint16_t bar_addr = wc_gadrw(&s_wnd, ROW_VGM_START, 2);
            ifl_pb_text_pg  = wc_get_pgc();   /* phys page of text screen */
            ifl_pb_attr_ofs = (bar_addr & 0x3FFF) | 0x0080; /* SET 7,L = attr area */
            ifl_pb_bw       = get_content_width(&s_wnd);
            ifl_pb_green    = WC_COLOR(WC_GREEN, WC_BLACK);
            ifl_pb_total    = 0;  /* will be set in load_vgm() */
        }
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

            /* ── 0x66 sentinel ───────────────────────────────────
             * Пишем VGM end-marker (0x66) сразу после конца данных,
             * чтобы hot-loop в vgm_fill_buffer мог работать без
             * проверки EOF на каждой итерации.                     */
            wc_mngcvpl(vgm_end_page);
            *(volatile uint8_t *)vgm_end_addr = 0x66;

            wc_mngcvpl(vgm_cur_page);

            /* Определяем типы чипов и длительность трека */
            detect_active_chips();
        }
    }

    drow_ui();

    /* Кешируем параметры progress bar (строка playback info) */
    {
        uint16_t bar_addr = wc_gadrw(&s_wnd, ROW_PROGRESS, 2);
        s_pb_text_pg  = wc_get_pgc();
        s_pb_char_ofs = bar_addr & 0x3FFF;
        s_pb_attr_ofs = s_pb_char_ofs | 0x0080;
        s_pb_width    = get_content_width(&s_wnd);
        s_pb_col      = 0;
        s_pb_err      = 0;
    }

    /* Кешируем адреса строк спектроанализатора */
    {
        for (uint8_t r = 0; r < 4; r++) {
            uint16_t a = wc_gadrw(&s_wnd, ROW_SPECTRUM + r, 2);
            s_spec_char_ofs[r] = a & 0x3FFF;
            s_spec_attr_ofs[r] = s_spec_char_ofs[r] | 0x0080;
        }
        spectrum_font_init();
        spectrum_decay_reset();
    }

    /* Печатаем полную строку progress bar ОДИН РАЗ через WC API.
     * "Playing  00:00 / MM:SS  Loop 1" + чёрный фон/белый текст.
     * В главном цикле обновляются только 5 символов + атрибуты. */
    {
        char tb[5];
        buf_clear(work_buf);
        buf_append_str(work_buf, "Playing  00:00 / ");
        fmt_mmss(tb, vgm_total_seconds);
        for (uint8_t i = 0; i < 5; i++) buf_append_char(work_buf, tb[i]);
        buf_append_str(work_buf, "  Loop 1");
        print_line(&s_wnd, ROW_PROGRESS, work_buf, WC_COLOR(WC_BRIGHT_WHITE, WC_BLACK));
    }

    if (state == STATE_PLAYBACK)
        start_playback();

    uint8_t key = 0;
    wc_exit_code = (wc_file_idx == wc_file_count) ? WC_EXIT_ESC : WC_EXIT_NEXT;

    /* FSM клавиатуры: стартуем в WAIT_RELEASE, чтобы дождаться
     * отпускания кнопки, оставшейся от предыдущего трека.     */
    kbd_init();

    /* Главный цикл: приоритет — update_buffer (буфер не должен голодать),
       клавиатура + UI — по остаточному принципу.                           */
    while (state == STATE_PLAYBACK)
    {
        /* ── 1. Буфер: максимальный приоритет ──────────────── */
        update_buffer();

        /* ISR замер на CMD_ISR_DONE → чистый выход */
        if (isr_done)
            break;

        /* ── 2. Клавиатура: FSM дебаунс (~3 мс) ───────────── */
        key = kbd_poll();

        if (key == KEY_NEXT)
        {
            vgm_hl_pos = vgm_hl_abort_pos;
            vgm_song_ended = 0;
            wc_exit_code = WC_EXIT_NEXT;
            instant_abort();
        }
        else if (key == KEY_PREV)
        {
            vgm_hl_pos = vgm_hl_abort_pos;
            vgm_song_ended = 0;
            wc_exit_code = WC_EXIT_PREV;
            instant_abort();
        }
        else if (key == KEY_ESC)
        {
            vgm_hl_pos = vgm_hl_abort_pos;
            vgm_song_ended = 0;
            wc_exit_code = WC_EXIT_ESC;
            instant_abort();
        }

        if (isr_play_seconds != s_last_displayed_sec || vgm_loop_count  != s_last_loop_count)
            update_playback_info();

        /* ── 3. Спектроанализатор ──────────────────────────── */
        spectrum_decay();
        spectrum_render();
    }

    stop_playback();
    spectrum_font_restore();
    wc_rresb(&s_wnd);
}

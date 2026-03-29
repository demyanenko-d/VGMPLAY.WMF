/**
 * @file    vgm.h
 * @brief   VGM-парсер для OPL1/OPL2/OPL3 — заголовок + потоковое чтение
 * @version 3.0
 *
 * Зависимости: types.h, isr.h (cmd_buf_a/b), wc_api.h (wc_mngcvpl).
 * Реализация:  lib/vgm.c.
 *
 * ═══════════════════════════════════════════════════════════════════
 * ОПИСАНИЕ
 * ═══════════════════════════════════════════════════════════════════
 *
 * Преобразует VGM-поток в ISR-команды (CMD_WRITE_B0/B1, CMD_WAIT,
 * CMD_END_BUF). VGM данные хранятся в VPL-страницах WC, доступных
 * через окно #C000.
 *
 * Поддерживаемые VGM-команды (OPL1 / OPL2 / OPL3 / AY / SAA):
 *   0x5A rr dd  — YM3812  (OPL2) write         → CMD_WRITE_B0
 *   0x5B rr dd  — YM3526  (OPL1) write         → CMD_WRITE_B0
 *   0x5C rr dd  — Y8950   (OPL)  write         → CMD_WRITE_B0
 *   0x5E rr dd  — YMF262  (OPL3) Bank 0 write  → CMD_WRITE_B0
 *   0x5F rr dd  — YMF262  (OPL3) Bank 1 write  → CMD_WRITE_B1
 *   0x55 rr dd  — YM2203  PSG-часть (AY), reg 0-15 → CMD_WRITE_AY
 *   0xA0 rr dd  — AY8910  (чип 1/2 по [7])  → CMD_WRITE_AY / CMD_WRITE_AY2
 *   0xBD rr dd  — SAA1099 (чип 1/2 по [7])  → CMD_WRITE_SAA / CMD_WRITE_SAA2
 *   0x61 nn nn  — Wait N samples (16-bit LE)
 *   0x62        — Wait 735 samples (1/60 сек)
 *   0x63        — Wait 882 samples (1/50 сек)
 *   0x66        — End of data
 *   0x67 ...    — Data block (пропускается)
 *   0x70–0x7F   — Wait 1–16 samples
 *   Прочие     — пропускаются по таблице VGM spec (1-байт / 2-байт / 3-байт аргументы)
 *
 * Масштабирование задержек:
 *   VGM sample rate = 44100 Hz, ISR freq = 2734 Hz -> делим на 16.
 *   Задержки < 16 сэмплов игнорируются (округление вниз).
 *   Неизвестные команды пропускаются по таблице размеров VGM spec.
 *
 * ═══════════════════════════════════════════════════════════════════
 * VGM ЗАГОЛОВОК — ключевые смещения (little-endian)
 * ═══════════════════════════════════════════════════════════════════
 *
 *   Offset  Size  Описание                          Мин.версия
 *   ------  ----  --------------------------------  ----------
 *   0x00    4     Сигнатура "Vgm " (0x206D6756)     1.00
 *   0x08    4     Версия (BCD): 0x0151 = v1.51      1.00
 *   0x14    4     GD3 offset (отн. 0x14)            1.00
 *   0x18    4     Total samples                      1.00
 *   0x1C    4     Loop offset (отн. 0x1C)           1.00
 *   0x20    4     Loop samples                       1.00
 *   0x34    4     Data offset (отн. 0x34)           1.50
 *   0x50    4     YM3812 (OPL2) clock               1.51
 *   0x54    4     YM3526 (OPL1) clock               1.51
 *   0x58    4     Y8950  (OPL)  clock               1.51
 *   0x5C    4     YMF262 (OPL3) clock               1.51
 *
 *   Для версий < 1.50: data offset фиксирован = 0x40.
 *   Для версий < 1.51: поля чипов за пределами заголовка.
 *
 *   ~93% OPL VGM файлов — YM3812 (OPL2), команда 0x5A.
 *   ~5%  — YMF262 (OPL3), команды 0x5E/0x5F.
 *
 * ═══════════════════════════════════════════════════════════════════
 * ПРИМЕР ИСПОЛЬЗОВАНИЯ
 * ═══════════════════════════════════════════════════════════════════
 *
 *   #include "vgm.h"
 *   #include "isr.h"
 *   #include "wc_api.h"
 *
 *   // 1. Загрузить VGM в VPL-страницы
 *   wc_mngcvpl(0);
 *   wc_load512(0xC000, 32);   // 16 КБ
 *
 *   // 2. Разобрать заголовок
 *   if (vgm_parse_header() != VGM_OK) return;
 *
 *   // 3. Заполнить оба буфера ISR
 *   vgm_fill_buffer(0);   // буфер A
 *   vgm_fill_buffer(1);   // буфер B
 *
 *   // 4. В main loop:
 *   if (cur_buf != last_buf) {
 *       vgm_fill_buffer(cur_buf ^ 1);  // свободный
 *       last_buf = cur_buf;
 *   }
 *   if (vgm_song_ended && !vgm_rewind_to_loop()) break;
 */

#ifndef VGM_H
#define VGM_H

#include "types.h"

/* ── Пересчёт частот под реальное железо (compile-time switch) ──────── */
#define VGM_FREQ_SCALE

#ifdef VGM_FREQ_SCALE
/** Режим масштабирования частот */
#define FREQ_MODE_NATIVE   0   /* HW clock ≈ VGM clock (±2%), no scaling */
#define FREQ_MODE_TABLE    1   /* LUT table match (±5%)                 */

/** Текущий режим масштабирования (FREQ_MODE_xxx) */
extern uint8_t vgm_freq_mode;

/** Базовый адрес LUT таблицы в Window 0 (#0000-#3FFF).
 *  Используется в TABLE mode: freq_lut_base[period] → scaled period.
 *  NULL (0) если режим не TABLE.
 *  PSG: индекс 0..4095 (12-bit), FM: индекс 0..2047 (11-bit).
 *  Таблица лежит в plugin page N, подключенной через wc_mng0_pl(N). */
extern uint16_t *freq_lut_base;
#endif /* VGM_FREQ_SCALE */

/* ══════════════════════════════════════════════════════════════════════
 * ID чипов = смещение clock-поля в VGM-заголовке.
 * Имена определены только для поддерживаемых чипов.
 * ══════════════════════════════════════════════════════════════════════ */
#define VGM_OFF_SN76489   0x0C   /* PSG SN76489 / SN76496              */
#define VGM_OFF_YM2413    0x10   /* OPLL                               */
#define VGM_OFF_YM2203    0x44   /* OPN   (PC-88)                      */
#define VGM_OFF_YM3812    0x50   /* OPL2  (AdLib / SB)                 */
#define VGM_OFF_YM3526    0x54   /* OPL   (ISA AdLib)                  */
#define VGM_OFF_Y8950     0x58   /* MSX-AUDIO / OPL                    */
#define VGM_OFF_YMF262    0x5C   /* OPL3  (SB Pro 2)                   */
#define VGM_OFF_YMF278B   0x60   /* OPL4                               */
#define VGM_OFF_AY8910    0x74   /* AY-3-8910 / YM2149                 */
#define VGM_OFF_SAA1099   0xC8   /* SAA1099 (SAM Coupé)                */

/* ── Тип OPL-чипа (обратная совместимость) ───────────────────────── */
#define VGM_CHIP_NONE  0
#define VGM_CHIP_OPL   1     /* YM3526 (OPL1) + Y8950: cmd 0x5B/0x5C  */
#define VGM_CHIP_OPL2  2     /* YM3812  (OPL2): cmd 0x5A              */
#define VGM_CHIP_OPL3  3     /* YMF262  (OPL3): cmd 0x5E/5F           */

/* Приоритет определения типа: YMF262 > YM3812 > YM3526/Y8950
 * Используется в start_playback() для выбора NEW=0/1 режима OPL3:
 *   VGM_CHIP_OPL3  → opl3_init() остаётся (NEW=1, L/R через C0-C8)
 *   не OPL3      → opl3_write_b1(OPL3_REG_OPL3EN, 0x00) (NEW=0, L+R авто) */

/* ── Коды результата ─────────────────────────────────────────────── */
#define VGM_OK         0
#define VGM_ERR_HEADER 1     /* Нет сигнатуры "Vgm " или повреждён     */
#define VGM_ERR_NOOPL  2     /* Нет OPL-чипа (clock = 0 для всех)      */

/* ── Флаги чипа (vgm_chip_entry_t.flags) ─────────────────────────── */
#define VGM_CF_DUAL    0x01  /* Bit 30 clock: два экземпляра чипа      */

/* ══════════════════════════════════════════════════════════════════════
 * Структура обнаруженного чипа
 * ══════════════════════════════════════════════════════════════════════ */
typedef struct {
    uint8_t  id;             /* VGM_OFF_xxx (header offset of clock)    */
    uint8_t  flags;          /* VGM_CF_DUAL и пр.                      */
    uint16_t clock_khz;      /* clock / 1000 (0–65535 кГц)             */
} vgm_chip_entry_t;          /* 4 байта                                */

#define VGM_MAX_CHIPS  8     /* макс. чипов в vgm_chip_list[]          */

/* ── Состояние парсера (глобальные переменные) ───────────────────── */

/** 1 = достигнут конец VGM-данных (команда 0x66 или конец файла) */
extern volatile uint8_t  vgm_song_ended;

/** 1 = пауза (буферы заполняются тишиной) */
extern uint8_t  vgm_paused;

/** Текущая VPL-страница, отображённая на #C000 */
extern uint8_t  vgm_cur_page;

/** Указатель чтения VGM (#C000–#FFFF) */
extern uint16_t vgm_read_ptr;

/** Номер последней VPL-страницы (0-based) */
extern uint8_t  vgm_end_page;

/** Адрес конца данных в последней странице */
extern uint16_t vgm_end_addr;

/** Версия VGM (BCD): 0x0151 = v1.51, 0x0171 = v1.71 */
extern uint16_t vgm_version;

/** Тип обнаруженного OPL-чипа (VGM_CHIP_xxx) */
extern uint8_t  vgm_chip_type;

/** Массив обнаруженных чипов (для вывода информации о файле) */
extern uint8_t          vgm_chip_count;
extern vgm_chip_entry_t vgm_chip_list[VGM_MAX_CHIPS];

/** Адрес точки петли (#C000–#FFFF); 0 = нет петли */
extern uint16_t vgm_loop_addr;

/** VPL-страница точки петли */
extern uint8_t  vgm_loop_page;

/* ── GD3 метаданные (только English, ASCII) ──────────────────────── */
#define VGM_GD3_LEN  48   /* макс. длина одного поля (+1 для '\0')   */

extern char vgm_gd3_track[VGM_GD3_LEN];   /* Track title (EN)          */
extern char vgm_gd3_game[VGM_GD3_LEN];    /* Game name (EN)            */
extern char vgm_gd3_system[VGM_GD3_LEN];  /* System name (EN)          */
extern char vgm_gd3_author[VGM_GD3_LEN];  /* Author (EN)               */

/* ── Функции ─────────────────────────────────────────────────────── */

/**
 * Разобрать заголовок VGM из #C000 (страница 0 должна быть загружена).
 * Заполняет vgm_read_ptr, vgm_cur_page, vgm_loop_addr/page,
 * vgm_version, vgm_chip_type, vgm_chip_list[]/vgm_chip_count.
 * @return VGM_OK (0) или VGM_ERR_HEADER (1)
 */
uint8_t vgm_parse_header(void);

/**
 * Заполнить командный буфер buf_idx (0 = A, 1 = B).
 *
 * Поддерживаемые VGM-команды:
 *   0x5A/0x5B/0x5C/0x5E → CMD_WRITE_B0 (OPL1/OPL2/Y8950/OPL3 bank 0)
 *   0x5F               → CMD_WRITE_B1 (OPL3 bank 1)
 *   0x55               → CMD_WRITE_AY (YM2203 PSG-часть, reg 0-15)
 *   0xA0               → CMD_WRITE_AY / CMD_WRITE_AY2 (AY8910, dual)
 *   0xBD               → CMD_WRITE_SAA / CMD_WRITE_SAA2 (SAA1099, dual)
 *   0x61/0x62/0x63/0x70-0x7F → CMD_WAIT
 *   0x66           → end; 0x67 → data block skip
 *
 * Гарантирует CMD_END_BUF в конце буфера.
 * @param buf_idx  0 = cmd_buf_a, 1 = cmd_buf_b
 */
void vgm_fill_buffer(uint8_t buf_idx);

/**
 * Перемотать VGM на точку петли (если установлена).
 * Сбрасывает vgm_song_ended = 0 при успехе.
 * @return 1 = петля есть (перемотано), 0 = нет петли
 */
uint8_t vgm_rewind_to_loop(void);

/**
 * Разобрать GD3-тег (метаданные) из VGM файла.
 * Вызывать после vgm_parse_header().
 * Заполняет vgm_gd3_track/game/system/author (ASCII, EN only).
 * Переключает VPL-страницу — после вызова нужно восстановить
 * vgm_cur_page через wc_mngcvpl().
 */
void vgm_parse_gd3(void);

/**
 * Получить имя чипа по header offset (VGM_OFF_xxx).
 * @return указатель на строку (ROM), например "YM3812", "AY8910".
 *         Для неизвестных чипов возвращает NULL.
 */
const char *vgm_chip_name(uint8_t id);

/* ─── VGM-format command blocks (cmdblocks page) ────────────────── */

/** Plugin page containing VGM-format command blocks */
#define CMDBLK_PAGE         5

/** Block indices in the pointer table at 0xC000 */
#define CMDBLK_INIT_OPL3    0   /* OPL3 init: NEW=1, 4-op=0, test=0   */
#define CMDBLK_INIT_OPL2    1   /* OPL2 compat: NEW=0, 4-op=0, test=0 */
#define CMDBLK_SILENCE_OPL  2   /* OPL silence: KeyOff+TL+waveform+BD */
#define CMDBLK_SILENCE_AY   3   /* AY chip 1: mixer off, vol=0        */
#define CMDBLK_SILENCE_AY2  4   /* AY chip 2 (TS): mixer off, vol=0   */
#define CMDBLK_SILENCE_SAA  5   /* SAA1099 chip 1: reset + amp=0      */
#define CMDBLK_SILENCE_SAA2 6   /* SAA1099 chip 2: reset + amp=0      */
#define CMDBLK_SAA_CLK_ON   7   /* MultiSound: SAA clock enable       */
#define CMDBLK_SAA_CLK_OFF  8   /* MultiSound: SAA clock disable      */
#define CMDBLK_SILENCE_YM2203   9 /* YM2203 chip 1: SSG+FM silence      */
#define CMDBLK_SILENCE_YM2203_2 10 /* YM2203 chip 2 (TS): SSG+FM silence */

/* ─── High-level command queue (HL queue) ───────────────────────── */

/** HL command types */
#define HLCMD_CMDBLK    1   /* Execute VGM-format cmdblock (param = CMDBLK_xxx) */
#define HLCMD_PLAY      2   /* Play VGM from current position until 0x66        */
#define HLCMD_LOOP      3   /* Rewind to loop point, then play (morphs→PLAY)    */
#define HLCMD_ISR_DONE  4   /* Emit CMD_ISR_DONE in buffer, ISR freezes         */

#define HL_QUEUE_MAX 16

typedef struct { uint8_t cmd, param; } hl_entry_t;

/** HL queue globals — written by main.c (build_playback_queue),
 *  consumed inside vgm_fill_buffer(). */
extern hl_entry_t vgm_hl_queue[HL_QUEUE_MAX];
extern uint8_t vgm_hl_len;        /* total entries              */
extern uint8_t vgm_hl_pos;        /* current entry              */
extern uint8_t vgm_hl_abort_pos;  /* jump-to on user abort      */
extern uint8_t vgm_loop_count;    /* incremented on each LOOP   */

/* ─── VGZ (gzip-сжатый VGM) ─────────────────────────────────────── */

/**
 * Копировать n_pages страниц из мегабуфера (#20+) в TAP (#A1+).
 * DI на время работы, восстанавливает Win 0 / Win 3 и EI при выходе.
 *
 * @param n_pages  количество страниц (макс. 31 — размер TAP области)
 */
void copy_pages_to_tap(uint8_t n_pages);

/**
 * Распаковать VGZ из TAP-страниц в мегабуфер.
 * Вызывается из C — ассемблерная обёртка (inflate_call.s).
 *
 * @param src_page  первая физ. страница источника (TAP, напр. 0xA1)
 * @param dst_page  первая физ. страница назначения (мегабуфер, напр. 0x20)
 * @return 0 — успех, 1 — ошибка (неверные данные / не gzip)
 */
uint8_t inflate_vgz(uint8_t src_page, uint8_t dst_page);

#endif /* VGM_H */

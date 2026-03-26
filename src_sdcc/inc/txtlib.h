/*
 * Структура буфера:
 *   buf[0]  = максимальная ёмкость (capacity) — задаётся ОДИН РАЗ при инициализации
 *   buf[1]  = текущая длина (len)
 *   buf[2..] = сами символы
 *
 * Объявление:
 *   char meta_buf[122];   // capacity = 120
 *   char work_buf[66];    // capacity = 64
 *   char tiny_buf[10];    // capacity = 8
 *
 */

#ifndef TXTLIB_H
#define TXTLIB_H

#include "wc_api.h"

#define FS_SCREEN_COLS 96U


static inline uint8_t get_content_width(const wc_window_t *win) {
    uint8_t w = win->width;
    return (w ? w : FS_SCREEN_COLS) - 4U;
}

/* ================================================================== */
/* ИНИЦИАЛИЗАЦИЯ И БАЗОВЫЕ ОПЕРАЦИИ                                   */
/* ================================================================== */

static inline void buf_init(char *buf, uint8_t capacity) {
    buf[0] = capacity;
    buf[1] = 0;
}

static inline void buf_clear(char *buf) {
    buf[1] = 0;
}

static inline uint8_t buf_len(const char *buf) {
    return buf[1];
}

/* ================================================================== */
/* APPEND                                                             */
/* ================================================================== */

uint8_t buf_append_str(char *buf, const char *src);
uint8_t buf_append_char(char *buf, char c);
uint8_t buf_append_u8_hex(char *buf, uint8_t val);
uint8_t buf_append_u16_dec(char *buf, uint16_t val);
uint8_t buf_append_u32_dec(char *buf, uint32_t val);


/* ================================================================== */
/* ВЫВОД                                                              */
/* ================================================================== */

void print_line(wc_window_t *win, uint8_t y, const char *buf, uint8_t attr);

/* ================================================================== */
/* ИНИЦИАЛИЗАЦИЯ И ПРИМЕР ИСПОЛЬЗОВАНИЯ                              */
/* ================================================================== */

/*
 // В начале плагина (например в init)
 char meta_buf[122];
 char work_buf[66];          // 64 символа

 buf_init(meta_buf, 120);
 buf_init(work_buf, 64);     // или больше, если используешь для marquee

 // Заполнение метаданных
 buf_clear(meta_buf);
 buf_append_str(meta_buf, "Artist - Super Long Track Name 2025 (Remix)");

 // В цикле отрисовки:
 uint8_t cw = get_content_width(my_win);

 // Бегущая строка (используем work_buf как временный)
 build_marquee(work_buf, meta_buf + 2, meta_buf[1], scroll_pos, cw);
 print_marquee(my_win, 3, work_buf, 0x0F);   // белый

 scroll_pos++;

 // Обычная строка (время)
 buf_clear(work_buf);
 buf_append_str(work_buf, "Time: ");
 buf_append_u16_dec(work_buf, minutes);
 buf_append_char(work_buf, ':');
 buf_append_u16_dec(work_buf, seconds);
 print_line(my_win, 5, work_buf, 0x0A);
*/

#endif // TXTLIB_H

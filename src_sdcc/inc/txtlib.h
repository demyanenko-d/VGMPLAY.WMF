/**
 * @file    txtlib.h
 * @brief   䇃䇐䈜䈛 для вывода текста в окна WC — динамические буферы и печать строк
 * @version 1.0
 *
 * Зависимости: wc_api.h (wc_prsrw, wc_window_t).
 * Реализация: lib/txtlib.c.
 *
 * Формат буфера:
 *   buf[0] = capacity  — максимальная емкость в символах (задаётся ОДИН РАЗ при buf_init)
 *   buf[1] = len       — текущая длина (количество символов)
 *   buf[2..] = символы   — не завершается \0!
 *
 * Объявление буфера:
 *   char work_buf[66];    // capacity = 64 символа
 *
 * Типичное использование:
 *   buf_init(work_buf, 64);
 *   buf_clear(work_buf);
 *   buf_append_str(work_buf, "Time: ");
 *   buf_append_u16_dec(work_buf, seconds);
 *   print_line(&win, 5, work_buf, WC_COLOR(WC_BLACK, WC_BRIGHT_WHITE));
 */

#ifndef TXTLIB_H
#define TXTLIB_H

#include "wc_api.h"

#define FS_SCREEN_COLS 96U  /* макс. ширина экрана (90x36, запас) */

/**
 * Вернуть (ширина окна - 4) = полезная ширина содержимого.
 * Если win->width == 0, используется FS_SCREEN_COLS.
 */
static inline uint8_t get_content_width(const wc_window_t *win) {
    uint8_t w = win->width;
    return (w ? w : FS_SCREEN_COLS) - 4U;
}

/* ================================================================== */
/* ИНИЦИАЛИЗАЦИЯ И БАЗОВЫЕ ОПЕРАЦИИ                                   */
/* ================================================================== */

/** Инициализировать буфер: записать capacity, len=0. Вызывать ОДИН РАЗ! */
static inline void buf_init(char *buf, uint8_t capacity) {
    buf[0] = capacity;
    buf[1] = 0;
}

/** Очистить буфер: len=0 (емкость не меняется). */
static inline void buf_clear(char *buf) {
    buf[1] = 0;
}

/** Вернуть текущую длину буфера (количество символов). */
static inline uint8_t buf_len(const char *buf) {
    return buf[1];
}

/* ================================================================== */
/* APPEND                                                             */
/* ================================================================== */

/**
 * Добавить строку (\0-terminated) в конец буфера.
 * Тихо усекается при переполнении (len не превышает capacity).
 * @return len после добавления
 */
uint8_t buf_append_str(char *buf, const char *src);

/**
 * Добавить один символ c в буфер.
 * @return len после добавления
 */
uint8_t buf_append_char(char *buf, char c);

/**
 * Добавить двухсимвольное hex-представление байта ("00".."FF").
 * @param val  значение 0..255
 * @return len после добавления (+2)
 */
uint8_t buf_append_u8_hex(char *buf, uint8_t val);

/**
 * Добавить uint16 в десятичном формате (без ведущих нулей).
 * @return len после добавления
 */
uint8_t buf_append_u16_dec(char *buf, uint16_t val);

/**
 * Добавить uint32 в десятичном формате.
 * @return len после добавления
 */
uint8_t buf_append_u32_dec(char *buf, uint32_t val);

/**
 * Добавить "MM:SS" (5 символов) в буфер. Без деления — вычитание.
 * @param sec  секунды (0..5999)
 */
void buf_append_mmss(char *buf, uint16_t sec);


/* ================================================================== */
/* ВЫВОД                                                              */
/* ================================================================== */

/**
 * Вывести одну строку из буфера в окно win заполняя до ширины окна.
 *  Передаётся атрибут цвета attr (EGA: bg[7:4] | fg[3:0]).
 *
 * @param win   окно WC
 * @param y     строка внутри окна (1-базированное)
 * @param buf   буфер с длиной в buf[1] (формат buf[])
 * @param attr  EGA-атрибут: WC_COLOR(bg, fg)
 */
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

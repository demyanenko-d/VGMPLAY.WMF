/**
 * @file    keys.h
 * @brief   PS/2 клавиатура через TSConfig CMOS FIFO (#EFF7/#DFF7/#BFF7)
 *
 * ps2_init()  — одноразовая инициализация: открытие PS/2 портов,
 *               активация FIFO, старт FSM в режиме DRAIN.
 * ps2_poll()  — вызывать из главного цикла; внутри FSM + rate-limit
 *               (~1 раз в TV-кадр).  Возвращает KEY_* при нажатии,
 *               KEY_NONE если нет новых событий.
 *
 * PS/2 Set 2 скан-коды:
 *   ESC   (#76) → KEY_ESC   — выход
 *   SPACE (#29) → KEY_NEXT  — следующий трек
 *   P     (#4D) → KEY_PREV  — предыдущий трек
 *   1     (#16) → KEY_SAA1  — SAA режим 1 (chip0)
 *   2     (#1E) → KEY_SAA2  — SAA режим 2 (chip1)
 */

#ifndef KEYS_H
#define KEYS_H

#include "types.h"

/* ── Коды клавиш (возврат ps2_poll) ──────────────────────────────────── */
#define KEY_NONE    0   /* нет нажатия                */
#define KEY_ESC     1   /* ESC         → выход         */
#define KEY_NEXT    2   /* SPACE       → следующий     */
#define KEY_PREV    3   /* P           → предыдущий    */
#define KEY_SAA1    4   /* 1           → SAA mode 1    */
#define KEY_SAA2    5   /* 2           → SAA mode 2    */
#define KEY_SAA3    6   /* 3           → SAA mode 3    */

/**
 * Инициализация PS/2: открыть порты, активировать FIFO, слив буфера.
 * Вызвать один раз при старте плагина (до первого ps2_poll).
 */
void ps2_init(void);

/**
 * Опросить PS/2 FIFO (rate-limited, ~1 раз в кадр).
 * Возвращает KEY_* при обнаружении нажатия, KEY_NONE иначе.
 * FSM обрабатывает протокол PS/2 (#F0/#E0 префиксы, overflow).
 */
uint8_t ps2_poll(void);

#endif /* KEYS_H */

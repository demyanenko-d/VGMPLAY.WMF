/**
 * @file    keys.h
 * @brief   PS/2 клавиатура через TSConfig CMOS FIFO (#EFF7/#DFF7/#BFF7)
 *
 * ps2_init()  — одноразовая инициализация: открытие PS/2 портов,
 *               активация FIFO, старт FSM в режиме DRAIN.
 * ps2_poll()  — вызывать из главного цикла; rate-limit (~1 раз в TV-кадр).
 *               За один вызов полностью сливает FIFO (tight loop).
 *               Возвращает последний найденный KEY_* или KEY_NONE.
 *
 * Единственный бит состояния — skip flag (после F0 пропустить 1 байт).
 * E0 не требует skip — сами коды не совпадают с нашими, а E0 F0 SC
 * корректно обрабатывается через F0→skip.
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
 * Деинициализация PS/2: слив FIFO, деактивация FIFO, отключение портов.
 * Вызвать при выходе из плагина (после главного цикла).
 */
void ps2_deinit(void);

/**
 * Опросить PS/2 FIFO (rate-limited, ~1 раз в кадр).
 * Клавиша подтверждается только после полного цикла make + release.
 * Возвращает KEY_* при подтверждении, KEY_NONE иначе.
 */
uint8_t ps2_poll(void);

#endif /* KEYS_H */

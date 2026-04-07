/**
 * @file    keys.h
 * @brief   Прямое чтение матрицы клавиш ZX Spectrum для VGM-плагина
 *
 * read_keys() опрашивает аппаратную матрицу клавиатуры через порты
 * #FE и возвращает код нажатой клавиши.
 *
 * Матрица: IN (C) при BC = ~(1<<row) в старшем байте | 0xFE в младшем.
 * Бит 0 ответа = нажата (инверсная логика, 0 = нажата).
 *
 * Опрашиваемые клавиши:
 *   NEXT  = SPACE (#7FFE бит 0) — следующий трек
 *   NEXT  = N (#7FFE бит 3)
 *   ESC   = Q (#FBFE бит 0) — выход
 *   PREV  = P (#DFFE бит 0)
 *   SAA1  = 1 (#F7FE бит 0) — SAA режим 1 (chip0)
 *   SAA2  = 2 (#F7FE бит 1) — SAA режим 2 (chip1)
 *   SAA3  = 3 (#F7FE бит 2) — SAA режим 3 (turbo)
 */

#ifndef KEYS_H
#define KEYS_H

#include "types.h"

/* ── Коды клавиш (возврат read_keys) ─────────────────────────────────── */
#define KEY_NONE    0   /* нет нажатия                */
#define KEY_ESC     1   /* Q           → выход         */
#define KEY_NEXT    2   /* SPACE или N → следующий     */
#define KEY_PREV    3   /* P           → предыдущий    */
#define KEY_SAA1    4   /* 1           → SAA mode 1    */
#define KEY_SAA2    5   /* 2           → SAA mode 2    */
#define KEY_SAA3    6   /* 3           → SAA mode 3    */

/**
 * Считать матрицу клавиш и вернуть код нажатой клавиши.
 * Возвращает KEY_NONE (0) если ничего не нажато.
 * Вызывать в главном цикле (polling), не из ISR.
 */
uint8_t read_keys(void) __naked;

#endif /* KEYS_H */

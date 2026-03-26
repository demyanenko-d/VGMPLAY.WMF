/**
 * @file    types.h
 * @brief   Базовые типы данных для Z80/SDCC (sdcccall(1))
 * @version 2.0
 *
 * Самодостаточный заголовок: не зависит от других файлов проекта.
 * Подходит для любого Z80-проекта на SDCC.
 *
 * ═══════════════════════════════════════════════════════════════════
 * SDCC sdcccall(1) CALLING CONVENTION  (Z80)
 * ═══════════════════════════════════════════════════════════════════
 *
 *   Аргумент    | Тип               | Регистр
 *   ------------|-------------------|----------
 *   1-й         | uint8_t / int8_t  | A
 *   1-й         | uint16_t / ptr    | DE
 *   2-й и далее | любой             | стек (правый первым)
 *   Возврат     | uint8_t           | A  (расширяется нулём в L)
 *   Возврат     | uint16_t / ptr    | HL
 *
 * ═══════════════════════════════════════════════════════════════════
 * РЕКОМЕНДАЦИИ ДЛЯ Z80
 * ═══════════════════════════════════════════════════════════════════
 *
 *   - uint32_t в hot-path -> десятки байт кода; избегать.
 *   - volatile — только для переменных, разделяемых с ISR.
 *   - __sfr / __xdata — НЕ использовать (это 8051, не Z80).
 *   - Маленькие inline-функции быстрее, чем call + стек.
 *   - Массивы до 256 элементов — индексировать uint8_t.
 *
 * ═══════════════════════════════════════════════════════════════════
 * ПРИМЕР
 * ═══════════════════════════════════════════════════════════════════
 *
 *   #include "types.h"
 *
 *   static volatile uint8_t flag;   // ISR -> main
 *   static uint16_t counter;
 *
 *   void timer_tick(void) {
 *       if (++counter >= 2734) { counter = 0; flag = 1; }
 *   }
 */

#ifndef TYPES_H
#define TYPES_H

/* ── Целочисленные типы фиксированной ширины ─────────────────────── */
typedef unsigned char  uint8_t;
typedef signed   char  int8_t;
typedef unsigned int   uint16_t;   /* SDCC Z80: int = 16 бит */
typedef signed   int   int16_t;
typedef unsigned long  uint32_t;   /* SDCC Z80: long = 32 бит */
typedef signed   long  int32_t;

/* ── Булев тип ───────────────────────────────────────────────────── */
/* uint8_t = один байт; сравнения JR Z / JR NZ без масок.           */
typedef uint8_t bool_t;
#define TRUE  ((bool_t)1)
#define FALSE ((bool_t)0)

/* ── NULL ────────────────────────────────────────────────────────── */
#ifndef NULL
#define NULL ((void *)0)
#endif

/* ── Короткие псевдонимы ─────────────────────────────────────────── */
typedef uint8_t  u8;
typedef uint16_t u16;
typedef int8_t   i8;
typedef int16_t  i16;
typedef uint32_t u32;
typedef int32_t  i32;

#endif /* TYPES_H */

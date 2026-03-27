/**
 * @file    opl3.h
 * @brief   OPL3 (YMF262) — константы регистров и функции записи
 * @version 2.0
 *
 * Самодостаточная библиотека: зависит только от types.h.
 * Реализация — asm/opl3.s (чистый inline OUT).
 *
 * ═══════════════════════════════════════════════════════════════════
 * ПОРТЫ TSConfig ДЛЯ OPL3
 * ═══════════════════════════════════════════════════════════════════
 *
 *   Порт  │ Описание
 *   ──────┼────────────────────────────
 *   #C4   │ Bank 0 Address (reg number)
 *   #C5   │ Bank 0 Data    (reg value)
 *   #C6   │ Bank 1 Address
 *   #C7   │ Bank 1 Data
 *
 * Задержка между записью адреса и данных НЕ нужна при 14 МГц
 * с wait-states: одна инструкция OUT (C),A занимает достаточно
 * тактов чтобы YMF262 обработал адрес до прихода данных.
 *
 * ═══════════════════════════════════════════════════════════════════
 * КАРТА РЕГИСТРОВ OPL3  (Bank 0, то же для Bank 1)
 * ═══════════════════════════════════════════════════════════════════
 *
 *   Рег.       │ Описание
 *   ───────────┼────────────────────────────────────────
 *   0x01       │ Wavesel enable (бит 5 = 1)
 *   0x04       │ Connection select (4-op mode, Bank 0)
 *   0x05 (B1)  │ OPL3 mode enable (бит 0 = 1)
 *   0x08       │ Note select / CSW (только Bank 0)
 *   0x20–0x35  │ Operator: AM/VIB/EG type/KSR/MULT
 *   0x40–0x55  │ Operator: KSL/Total Level (громкость)
 *   0x60–0x75  │ Operator: Attack Rate/Decay Rate
 *   0x80–0x95  │ Operator: Sustain Level/Release Rate
 *   0xA0–0xA8  │ Channel: F-Number low 8 bits
 *   0xB0–0xB8  │ Channel: Key-On + Octave + F-Number hi
 *   0xBD       │ Percussion mode / AM depth / VIB depth
 *   0xC0–0xC8  │ Channel: Feedback + Algorithm + Output
 *   0xE0–0xF5  │ Operator: Waveform select
 *
 * ═══════════════════════════════════════════════════════════════════
 * ПРИМЕР
 * ═══════════════════════════════════════════════════════════════════
 *
 *   #include "opl3.h"
 *
 *   void play_note(void) {
 *       opl3_init();                        // включить OPL3
 *       opl3_write_b0(0x20, 0x01);          // op1: MULT=1
 *       opl3_write_b0(0x40, 0x10);          // op1: TL=16
 *       opl3_write_b0(0x60, 0xF0);          // op1: AR=F, DR=0
 *       opl3_write_b0(0x80, 0x77);          // op1: SL=7, RR=7
 *       opl3_write_b0(0xA0, 0x98);          // F-num lo
 *       opl3_write_b0(0xB0, 0x31);          // Key-On, Oct=4, F-hi
 *   }
 */

#ifndef OPL3_H
#define OPL3_H

#include "types.h"

/* ── Порты OPL3 на шине TSConfig ─────────────────────────────────── */
#define OPL3_ADDR0  0xC4    /* Bank 0 register address */
#define OPL3_DATA0  0xC5    /* Bank 0 register data    */
#define OPL3_ADDR1  0xC6    /* Bank 1 register address */
#define OPL3_DATA1  0xC7    /* Bank 1 register data    */

/* ── Нумерация регистров (часто используемые) ────────────────────── */
#define OPL3_REG_WAVESEL    0x01    /* Waveform select enable (бит 5)  */
#define OPL3_REG_CONNSEL    0x04    /* Connection select (4-op, Bank0) */
#define OPL3_REG_OPL3EN     0x05    /* OPL3 mode reg (Bank 1 only!)   */
#define OPL3_OPL3EN_ON      0x01    /* NEW=1: OPL3 mode (L/R via C0-C8)*/
#define OPL3_OPL3EN_OFF     0x00    /* NEW=0: OPL2 compat (L+R always)  */
#define OPL3_REG_CSW        0x08    /* CSW / Note select (Bank 0)     */
#define OPL3_REG_BD         0xBD    /* Percussion / AM / VIB depth    */

/* Key-On бит в регистрах 0xB0-0xB8 */
#define OPL3_KEY_ON         0x20

/* ── Функции (реализация: asm/opl3.s) ────────────────────────────── */

/** Записать регистр Bank 0: addr -> #C4, data -> #C5 */
void opl3_write_b0(uint8_t addr, uint8_t data);

/** Записать регистр Bank 1: addr -> #C6, data -> #C7 */
void opl3_write_b1(uint8_t addr, uint8_t data);

/**
 * Инициализация OPL3:
 *   1. Bank1 reg[0x05] = 0x01  — NEW=1, OPL3 mode (регистры C0-C8 управляют L/R)
 *   2. Bank0 reg[0x01] = 0x20  — Waveform Select Enable
 *   3. Полный сброс всех 18 каналов: KeyOff, TL, F-Num (opl3_reset)
 *
 * ⚠ После `opl3_init()` чип находится в режиме OPL3 (NEW=1):
 *   - Регистры C0-C8 задают маршрутизацию L/R (биты 4-5)
 *   - OPL2/OPL1 VGM файлы НЕ устанавливают биты L/R → тишина!
 *
 * Для OPL1/OPL2 файлов после init вызовите:
 *   opl3_write_b1(OPL3_REG_OPL3EN, 0x00);  // NEW=0: OPL2-compat mode
 * В этом режиме L+R включены автоматически, биты C0-C8[5:4] игнорируются.
 */
void opl3_init(void);

/** Заглушить все 18 каналов (KeyOff: B0-B8 = 0, обе банки) */
void opl3_reset(void);

/** AY-3-8910 / YM2149 chip 1: mixer off + volume 0 */
void ay_silence(void);

/** AY-3-8910 chip 2 (TurboSound): select chip2, silence, switch back */
void ay2_silence(void);

/** AY chip 1: write single register */
void ay_write_reg(uint8_t reg, uint8_t val);

/** AY chip 2 (TurboSound): write single register */
void ay2_write_reg(uint8_t reg, uint8_t val);

/** SAA1099 chip 1: reset + disable + zero amplitudes */
void saa_silence(void);

/** SAA1099 chip 2: reset + disable + zero amplitudes */
void saa2_silence(void);

/** SAA1099 chip 1: write single register */
void saa_write_reg(uint8_t reg, uint8_t val);

/** SAA1099 chip 2: write single register */
void saa2_write_reg(uint8_t reg, uint8_t val);

#endif /* OPL3_H */

/**
 * @file    isr.h
 * @brief   ISR (Interrupt Service Routine) для FRAME-прерывания TSConfig
 * @version 2.0
 *
 * Зависимости: types.h.
 * Реализация:  asm/isr.s (чистый ASM, hot-path без C).
 *
 * ═══════════════════════════════════════════════════════════════════
 * АРХИТЕКТУРА
 * ═══════════════════════════════════════════════════════════════════
 *
 * ISR работает на частоте 1367 Гц (= 44100/32 = 3500000/2560).
 * 28 прерываний x 2560 T-states = 71680 T = 1 TV-кадр (замыкание).
 *
 * Позиции INT — предрассчитанная таблица POS_TABLE (56 x 5 = 280 байт).
 * ISR читает 3 байта через OUTI и записывает в VSINTH/VSINTL/HSINT,
 * затем переходит по next_ptr (замкнутый цикл).
 *
 * ═══════════════════════════════════════════════════════════════════
 * ДВОЙНАЯ БУФЕРИЗАЦИЯ КОМАНД
 * ═══════════════════════════════════════════════════════════════════
 *
 *   CMD_BUF_A, CMD_BUF_B — по 512 байт (до 128 команд).
 *   ISR читает из активного буфера, main loop заполняет свободный.
 *   Переключение буфера: CMD_END_BUF в конце каждого заполненного.
 *
 * ═══════════════════════════════════════════════════════════════════
 * ФОРМАТ КОМАНДЫ (4 байта, выровнено)
 * ═══════════════════════════════════════════════════════════════════
 *
 *   Код          │ Байт 1 │ Байт 2 │ Байт 3 │ Действие
 *   ─────────────┼────────┼────────┼────────┼─────────────────────
 *   CMD_WRITE_AY │ reg    │ val    │ 0      │ OUT AY8910 chip 1
 *   CMD_INC_SEC  │ 0      │ 0      │ 0      │ Инкр. isr_play_seconds
 *   CMD_WRITE_AY2│ reg    │ val    │ 0      │ OUT AY8910 chip 2 (TS)
 *   CMD_CALL_WC  │ 0      │ 0      │ 0      │ Вызов WC ISR handler
 *   CMD_WRITE_B0 │ reg    │ val    │ 0      │ OUT OPL3 Bank 0
 *   CMD_SKIP_TICKS│ N     │ 0      │ 0      │ Пропустить N позиций pos_table
 *   CMD_WRITE_SAA│ reg    │ val    │ 0      │ OUT SAA1099 chip 1
 *   CMD_WRITE_B1 │ reg    │ val    │ 0      │ OUT OPL3 Bank 1
 *   CMD_WRITE_SA2│ reg    │ val    │ 0      │ OUT SAA1099 chip 2
 *   CMD_WAIT     │ lo     │ hi     │ 0      │ Ждать (hi<<8)|lo тиков
 *   CMD_END_BUF  │ 0      │ 0      │ 0      │ Переключить буфер
 *
 * ═══════════════════════════════════════════════════════════════════
 * ПРИМЕР ИСПОЛЬЗОВАНИЯ
 * ═══════════════════════════════════════════════════════════════════
 *
 *   #include "isr.h"
 *
 *   // 1. Заполнить оба буфера
 *   fill_buffer(cmd_buf_a);
 *   fill_buffer(cmd_buf_b);
 *
 *   // 2. Настроить ISR
 *   isr_enabled    = 0;     // пока не запускать
 *   isr_active_buf = 0;     // начать с буфера A
 *   isr_init();              // IM2 + таблица векторов
 *
 *   // 3. Запустить
 *   isr_enabled = 1;
 *   __asm__("ei");
 *
 *   // 4. В main loop:
 *   if (isr_active_buf != last_buf) {
 *       fill_buffer(isr_active_buf ? cmd_buf_a : cmd_buf_b);
 *       last_buf = isr_active_buf;
 *   }
 */

#ifndef ISR_H
#define ISR_H

#include "types.h"

/* ── Коды команд (8 типов, шаг 0x20 + ISR_DONE) ─────────────────── */
#define CMD_WRITE_AY  0x00  /* AY8910 chip 1: [reg, val, 0]          */
#define CMD_INC_SEC   0x10  /* Инкрементировать счётчик секунд: [0,0,0]    */
#define CMD_WRITE_AY2 0x20  /* AY8910 chip 2 (TurboSound): [reg,val,0]*/
#define CMD_CALL_WC   0x30  /* Вызов WC ISR handler: [0,0,0]        */
#define CMD_WRITE_B0  0x40  /* OPL3 Bank 0 write: [reg, val, 0]      */
#define CMD_SKIP_TICKS 0x50 /* Пропустить N записей pos_table: [N,0,0]  */
#define CMD_WRITE_SAA 0x60  /* SAA1099 chip 1: [reg, val, 0]         */
#define CMD_WRITE_B1  0x80  /* OPL3 Bank 1 write: [reg, val, 0]      */
#define CMD_WRITE_SAA2 0xA0 /* SAA1099 chip 2: [reg, val, 0]         */
#define CMD_WAIT      0xC0  /* Ждать N ISR-тиков:  [lo,  hi,  0]      */
#define CMD_END_BUF   0xE0  /* Переключить буфер: [0,   0,   0]      */
#define CMD_ISR_DONE  0xF0  /* Заморозить ISR, выставить isr_done: [0,0,0] */

/* ── Размер командного буфера ────────────────────────────────────── */
#define CMD_BUF_SIZE  512   /* байт; 128 команд по 4 байта           */

/* ── Параметры варианта (ISR_FREQ, VGM_SAMPLE_*, budget) ───── */
#include "variant_cfg.h"

/* ── Переменные ISR (volatile = разделяемые между ISR и main) ────── */

/** Индекс активного буфера: 0 = A, 1 = B.
 *  Записывается ISR при CMD_END_BUF, читается main loop. */
extern volatile uint8_t  isr_active_buf;

/** Флаг включения: 0 = ISR проверяет, но НЕ выполняет команды.
 *  Записывается main loop, читается ISR. */
extern volatile uint8_t  isr_enabled;

/** Счётчик тиков ISR. Инкрементируется каждые ~365 мкс (2734 Гц),
 *  оборачивается при 256 (~93 мс полный оборот).
 *  main loop читает для определения прошедшего времени. */
extern volatile uint8_t  isr_tick_ctr;

/** Цвет бордюра, восстанавливаемый ISR при выходе.
 *  Записывается main loop (0=black, 2=red и т.д.), читается ISR. */
extern volatile uint8_t  isr_border_color;

/** Счётчик секунд воспроизведения.
 *  Инкрементируется ISR по команде CMD_INC_SEC, читается main loop. */
extern volatile uint16_t isr_play_seconds;

/** Флаг завершения: 1 = ISR заморожен на CMD_ISR_DONE.
 *  Записывается ISR, читается main loop.
 *  Main loop очищает при старте воспроизведения. */
extern volatile uint8_t  isr_done;

/** Командные буферы A и B. Заполняются main loop, читаются ISR. */
extern uint8_t cmd_buf_a[CMD_BUF_SIZE];
extern uint8_t cmd_buf_b[CMD_BUF_SIZE];

/** Контрольный счётчик текущей задержки CMD_WAIT.
 *  ISR декрементирует при каждом тике, переходит к следующей команде после достижения 0.
 *  Не используется напрямую main loop — только ISR. */
extern volatile uint16_t isr_wait_ctr;

/** Указатель чтения ISR внутри активного буфера. */
extern volatile uint16_t isr_read_ptr;

/* ── Функции ─────────────────────────────────────────────────────── */

/**
 * Перехват вектора IM2 (#5BFF) и настройка 2734 Hz.
 *
 * Действия:
 *   1. Сохранить вектор WC (#5BFF) → isr_saved_vec
 *   2. Записать адрес isr_handler в #5BFF
 *   3. Настроить INTMASK = FRAME only, позиции INT
 *   4. Инициализировать pos_ptr, wait_ctr, tick_ctr, read_ptr
 *   5. IM 2  (EI делается вызывающим кодом)
 *
 * ВАЖНО: вызывать ПОСЛЕ заполнения обоих буферов!
 * ВАЖНО: вызывать при DI (прерывания выключены).
 */
void isr_init(void);

/**
 * Восстановить вектор WC и сбросить INT-позиции.
 *
 * Действия:
 *   1. Восстановить оригинальный вектор из isr_saved_vec → #5BFF
 *   2. Сбросить позицию INT на line=0 (один INT/кадр)
 *   3. INTMASK = FRAME only (WC default)
 *   НЕ делает EI — вызывающий код решает сам.
 *
 * Безопасно вызывать без предшествующего isr_init (no-op).
 */
void isr_deinit(void);

/**
 * Вызов оригинального WC ISR handler из main loop.
 *
 * WC ожидает периодический вызов своего ISR (обновление таймера,
 * клавиатуры, часов).  Вызывать ~50 Гц из main loop.
 * Делает DI → JP в WC handler → WC делает EI+RET.
 * Безопасно вызывать без предшествующего isr_init (no-op).
 */
void call_wc_handler(void);

#endif /* ISR_H */

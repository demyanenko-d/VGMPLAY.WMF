/**
 * @file    keys.c
 * @brief   PS/2 клавиатура через TSConfig CMOS FIFO (порты #EFF7/#DFF7/#BFF7)
 * @see     inc/keys.h
 *
 * Аппаратный PS/2 FIFO буфер в FPGA TSConfig, регистр CMOS #F0.
 *
 * ps2_poll() за один вызов полностью сливает FIFO (tight loop).
 * Если среди прочитанных байтов есть make-код нашей клавиши — возвращает
 * последний найденный KEY_*.  Стая байтов никогда не копится.
 *
 * Единственный бит состояния — skip flag (после F0 нужно пропустить 1 байт).
 * E0 (extended prefix) не требует skip: сам E0 не совпадает с нашими кодами,
 * а release extended (E0 F0 SC) корректно обрабатывается через F0→skip.
 *
 * Опрос ~1 раз в TV-кадр (rate-limited по isr_tick_ctr).
 * ps2_poll полностью на asm.
 */

#include "../inc/types.h"
#include "../inc/keys.h"
#include "../inc/isr.h"          /* isr_tick_ctr (volatile uint8_t)      */
#include "../inc/variant_cfg.h"  /* ISR_TICKS_PER_FRAME                  */

/* Единственная FSM-переменная: skip flag (0 или 1).                   */
static uint8_t ps2_state;        /* 0 = normal, 1 = skip next byte      */
static uint8_t ps2_last_tick;

/* ── ps2_init: активация PS/2 FIFO + одноразовый слив ────────────── */
void ps2_init(void)
{
    __asm
        ; Разрешить PS/2 порты (бит 7 #EFF7)
        ld   bc, #0xEFF7
        ld   a, #0x80
        out  (c), a
        ; Выбрать CMOS reg #F0, записать 2 (активация), re-select #F0
        ld   bc, #0xDFF7
        ld   a, #0xF0
        out  (c), a
        ld   bc, #0xBFF7
        ld   a, #0x02
        out  (c), a
        ld   bc, #0xDFF7
        ld   a, #0xF0
        out  (c), a
        ; Слив FIFO (tight loop, макс 256 итераций)
        ld   bc, #0xBFF7
        ld   e, #0
    _pi_drain:
        in   a, (c)
        or   a, a
        jr   z, _pi_done        ; 0x00 = пусто
        inc  a
        jr   z, _pi_ovf         ; 0xFF = overflow
        dec  e
        jr   nz, _pi_drain
        jr   _pi_done
    _pi_ovf:
        ld   bc, #0xDFF7
        ld   a, #0x0C
        out  (c), a
        ld   bc, #0xBFF7
        ld   a, #0x01
        out  (c), a
        ld   bc, #0xDFF7
        ld   a, #0xF0
        out  (c), a
    _pi_done:
    __endasm;
    ps2_state     = 0;
    ps2_last_tick = isr_tick_ctr;
}

/* ── ps2_deinit: слив FIFO перед выходом ──────────────────────────── */
void ps2_deinit(void)
{
    __asm
        ld   bc, #0xDFF7
        ld   a, #0xF0
        out  (c), a
        ld   bc, #0xBFF7
        ld   e, #0
    _pd_loop:
        in   a, (c)
        or   a, a
        jr   z, _pd_done        ; 0x00 = пусто
        inc  a
        jr   z, _pd_ovf         ; 0xFF = overflow
        dec  e
        jr   nz, _pd_loop
        jr   _pd_done
    _pd_ovf:
        ld   bc, #0xDFF7
        ld   a, #0x0C
        out  (c), a
        ld   bc, #0xBFF7
        ld   a, #0x01
        out  (c), a
        ld   bc, #0xDFF7
        ld   a, #0xF0
        out  (c), a
    _pd_done:
    __endasm;
}

/* ── ps2_poll: tight-drain FIFO, rate-limited, полностью asm ──────── */
uint8_t ps2_poll(void) __naked
{
    __asm

        ;; ── Rate limit: ~1 TV-кадр ────────────────────────
        ld   a, (_isr_tick_ctr)
        ld   hl, #_ps2_last_tick
        ld   e, a
        sub  a, (hl)
        cp   a, #ISR_TICKS_PER_FRAME
        jp   c, _pp_none
        ld   (hl), e

        ;; ── Подготовка: выбрать FIFO, init регистров ──────
        ld   bc, #0xDFF7
        ld   a, #0xF0
        out  (c), a
        ld   bc, #0xBFF7             ; BC = порт FIFO на весь цикл
        ld   a, (_ps2_state)
        ld   d, a                    ; D = skip flag (от пред. вызова)
        ld   e, #0                   ; E = результат (KEY_NONE)

        ;; ── Цикл: читаем FIFO до пустого ─────────────────
    _pp_loop:
        in   a, (c)                  ; BC = #BFF7
        or   a, a
        jr   z, _pp_done            ; 0x00 = пусто → выход
        inc  a
        jr   z, _pp_ovf             ; 0xFF = overflow
        dec  a                       ; восстановить байт

        bit  0, d
        jr   nz, _pp_clr_skip       ; skip-флаг → пропустить байт

        cp   a, #0xF0
        jr   z, _pp_set_skip        ; F0 → пропустить след. байт

        ;; ── Трансляция scan code → KEY_* ──────────────────
        cp   a, #0x76               ; ESC
        jr   z, _pp_k_esc
        cp   a, #0x29               ; SPACE
        jr   z, _pp_k_next
        cp   a, #0x4D               ; P
        jr   z, _pp_k_prev
        cp   a, #0x16               ; 1
        jr   z, _pp_k_saa1
        cp   a, #0x1E               ; 2
        jr   z, _pp_k_saa2
        jr   _pp_loop               ; неизвестная → продолжаем слив

    _pp_k_esc:  ld   e, #KEY_ESC
                jr   _pp_loop
    _pp_k_next: ld   e, #KEY_NEXT
                jr   _pp_loop
    _pp_k_prev: ld   e, #KEY_PREV
                jr   _pp_loop
    _pp_k_saa1: ld   e, #KEY_SAA1
                jr   _pp_loop
    _pp_k_saa2: ld   e, #KEY_SAA2
                jr   _pp_loop

    _pp_set_skip:
        ld   d, #1
        jr   _pp_loop

    _pp_clr_skip:
        ld   d, #0
        jr   _pp_loop

        ;; ── Overflow: сброс FIFO ─────────────────────────
    _pp_ovf:
        ld   bc, #0xDFF7
        ld   a, #0x0C
        out  (c), a
        ld   bc, #0xBFF7
        ld   a, #0x01
        out  (c), a
        ld   bc, #0xDFF7
        ld   a, #0xF0
        out  (c), a
        ld   d, #0                   ; сбросить skip
        ;; fall-through

        ;; ── Готово: сохранить skip, вернуть KEY ───────────
    _pp_done:
        ld   a, d
        ld   (_ps2_state), a        ; skip flag для след. вызова
        ld   a, e                    ; A = KEY_* (sdcccall(1))
        ret

    _pp_none:
        xor  a, a                    ; KEY_NONE = 0
        ret

    __endasm;
}
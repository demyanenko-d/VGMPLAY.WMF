/**
 * @file    keys.c
 * @brief   PS/2 клавиатура через TSConfig CMOS FIFO (порты #EFF7/#DFF7/#BFF7)
 * @see     inc/keys.h
 *
 * Аппаратный PS/2 FIFO буфер в FPGA TSConfig, регистр CMOS #F0.
 *
 * FSM (минимальная работа за вызов, ~1 раз в TV-кадр):
 *   ST_DRAIN (0) — слив буфера (после init / принятой клавиши)
 *   ST_READY (1) — приём make-кодов
 *   ST_SKIP  (2) — пропуск байта после #F0 (release) / #E0 (extended)
 *
 * ps2_poll полностью на asm: инлайн чтения FIFO, overflow reset,
 * трансляция скан-кодов — без call overhead.
 */

#include "../inc/types.h"
#include "../inc/keys.h"
#include "../inc/isr.h"          /* isr_tick_ctr (volatile uint8_t)      */
#include "../inc/variant_cfg.h"  /* ISR_TICKS_PER_FRAME                  */

/* FSM-переменные.  C-объявления гарантируют аллокацию в _DATA;
 * ps2_init обращается к ним из C, ps2_poll — из inline asm.          */
static uint8_t ps2_state;
static uint8_t ps2_last_tick;
static uint8_t ps2_empty_cnt;

/* ── ps2_init: одноразовая инициализация ─────────────────────────── */
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
    __endasm;
    ps2_state     = 0;              /* ST_DRAIN */
    ps2_last_tick = isr_tick_ctr;
    ps2_empty_cnt = 0;
}

/* ── ps2_poll: FSM + rate-limited read (полностью asm) ───────────── */
uint8_t ps2_poll(void) __naked
{
    __asm

        ;; ── Rate limit: ~1 TV-кадр ────────────────────────
        ld   a, (_isr_tick_ctr)
        ld   e, a                      ; сохранить tick
        ld   hl, #_ps2_last_tick
        sub  a, (hl)
        cp   a, #ISR_TICKS_PER_FRAME
        jp   c, _pp_none
        ld   (hl), e                   ; обновить last_tick

        ;; ── Прочитать FIFO byte → D ───────────────────────
        ld   bc, #0xDFF7
        ld   a, #0xF0
        out  (c), a
        ld   bc, #0xBFF7
        in   d, (c)

        ;; ── Диспетчер по ps2_state ────────────────────────
        ld   a, (_ps2_state)
        or   a, a
        jr   z, _pp_drain             ; 0 = ST_DRAIN
        dec  a
        jr   z, _pp_ready             ; 1 = ST_READY
                                       ; 2 = ST_SKIP (fall-through)

        ;; ── ST_SKIP ───────────────────────────────────────
        ld   a, d
        or   a, a
        jp   z, _pp_none              ; пусто — ждём
        cp   a, #0xF0
        jp   z, _pp_none              ; release prefix
        cp   a, #0xE0
        jp   z, _pp_none              ; extended prefix
        inc  a                         ; 0xFF+1=0 → overflow?
        jr   z, _pp_ovf_drain
        ld   a, #1                     ; consumed → ST_READY
        ld   (_ps2_state), a
        jp   _pp_none

        ;; ── ST_DRAIN ──────────────────────────────────────
    _pp_drain:
        ld   a, d
        or   a, a
        jr   nz, _pp_dr_ne
        ;; пусто — считаем пустые подряд
        ld   hl, #_ps2_empty_cnt
        inc  (hl)
        ld   a, (hl)
        cp   a, #3
        jp   c, _pp_none              ; < 3 → ждём
        ld   a, #1                     ; → ST_READY
        ld   (_ps2_state), a
        jp   _pp_none
    _pp_dr_ne:
        xor  a, a
        ld   (_ps2_empty_cnt), a
        ld   a, d
        inc  a                         ; 0xFF → overflow?
        jp   nz, _pp_none
        call _pp_ovf_rst
        jp   _pp_none

        ;; ── ST_READY ──────────────────────────────────────
    _pp_ready:
        ld   a, d
        or   a, a
        jr   z, _pp_none              ; пусто
        inc  a                         ; 0xFF → 0?
        jr   z, _pp_ovf_drain
        ld   a, d
        cp   a, #0xF0
        jr   z, _pp_to_skip
        cp   a, #0xE0
        jr   z, _pp_to_skip

        ;; ── Трансляция скан-кода (cp не меняет A) ─────────
        cp   a, #0x76                  ; ESC
        jr   z, _pp_k1
        cp   a, #0x29                  ; SPACE
        jr   z, _pp_k2
        cp   a, #0x4D                  ; P
        jr   z, _pp_k3
        cp   a, #0x16                  ; 1
        jr   z, _pp_k4
        cp   a, #0x1E                  ; 2
        jr   z, _pp_k5
        jr   _pp_none                  ; неизвестная клавиша

    _pp_k1: ld   a, #KEY_ESC
            jr   _pp_accept
    _pp_k2: ld   a, #KEY_NEXT
            jr   _pp_accept
    _pp_k3: ld   a, #KEY_PREV
            jr   _pp_accept
    _pp_k4: ld   a, #KEY_SAA1
            jr   _pp_accept
    _pp_k5: ld   a, #KEY_SAA2
            ;; fall-through

    _pp_accept:
        ld   e, a                      ; сохранить KEY_*
        xor  a, a
        ld   (_ps2_state), a           ; → ST_DRAIN
        ld   (_ps2_empty_cnt), a
        ld   a, e
        ret

    _pp_to_skip:
        ld   a, #2                     ; → ST_SKIP
        ld   (_ps2_state), a
        jr   _pp_none

    _pp_ovf_drain:
        call _pp_ovf_rst
        xor  a, a
        ld   (_ps2_state), a
        ld   (_ps2_empty_cnt), a
        ;; fall-through

    _pp_none:
        xor  a, a                      ; KEY_NONE = 0
        ret

        ;; ── overflow reset (subroutine) ───────────────────
    _pp_ovf_rst:
        ld   bc, #0xDFF7
        ld   a, #0x0C
        out  (c), a
        ld   bc, #0xBFF7
        ld   a, #0x01
        out  (c), a
        ld   bc, #0xDFF7
        ld   a, #0xF0
        out  (c), a
        ret

    __endasm;
}
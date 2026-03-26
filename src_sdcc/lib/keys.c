/**
 * @file    keys.c
 * @brief   Прямое чтение матрицы клавиш ZX Spectrum (VGM-плагин)
 * @see     inc/keys.h
 */

#include "../inc/types.h"
#include "../inc/keys.h"

/* ── Чтение матрицы ──────────────────────────────────────────────────── */
uint8_t read_keys(void) __naked
{
    __asm
        ; --- SPACE (#7FFE бит 0) → ESC ---
        ld      bc, #0x7FFE
        in      a, (c)
        bit     0, a
        jr      nz, _rk_no_space

        ld      a, #KEY_ESC         ; sdcccall(1): uint8_t return → A
        ret
_rk_no_space:
        ; --- N (#7FFE бит 3): следующий файл ---
        ld      bc, #0x7FFE
        in      a, (c)
        bit     3, a
        jr      nz, _rk_no_n
        ld      a, #KEY_NEXT
        ret
_rk_no_n:
        ; --- P (#DFFE бит 0): предыдущий файл ---
        ld      bc, #0xDFFE
        in      a, (c)
        bit     0, a
        jr      nz, _rk_no_p
        ld      a, #KEY_PREV
        ret
_rk_no_p:
        ; --- Q (#FBFE бит 0): альтернативный выход ---
        ld      bc, #0xFBFE
        in      a, (c)
        bit     0, a
        jr      nz, _rk_none
        ld      a, #KEY_ESC
        ret
_rk_none:
        xor     a, a                ; A = KEY_NONE = 0
        ret
    __endasm;
}

/* ── Антидребезг ─────────────────────────────────────────────────────── */
void debounce(void)
{
    volatile uint16_t c = 6000U;
    while (c--) ;
}

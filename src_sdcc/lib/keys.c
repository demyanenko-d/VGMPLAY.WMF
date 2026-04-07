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
        ; --- SPACE (#7FFE бит 0) или N (#7FFE бит 3): следующий файл ---
        ld      bc, #0x7FFE
        in      a, (c)
        bit     0, a
        jr      nz, _rk_no_space
        ld      a, #KEY_NEXT        ; sdcccall(1): uint8_t return → A
        ret
_rk_no_space:
        ld      a, #0x7F            ; re-read same port row (already in C)
        in      a, (#0xFE)
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
        ; --- Q (#FBFE бит 0): выход ---
        ld      bc, #0xFBFE
        in      a, (c)
        bit     0, a
        jr      nz, _rk_no_q
        ld      a, #KEY_ESC
        ret
_rk_no_q:
        ; --- 1,2,3 (#F7FE биты 0,1,2): SAA mode ---
        ld      bc, #0xF7FE
        in      a, (c)
        ld      d, #KEY_SAA1
        bit     0, a
        jr      z, _rk_saa_hit
        inc     d
        bit     1, a
        jr      z, _rk_saa_hit
        inc     d
        bit     2, a
        jr      nz, _rk_none
_rk_saa_hit:
        ld      a, d
        ret
_rk_none:
        xor     a, a                ; A = KEY_NONE = 0
        ret
    __endasm;
}



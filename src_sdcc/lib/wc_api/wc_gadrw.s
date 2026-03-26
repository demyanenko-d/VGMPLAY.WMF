        .module wc_gadrw
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_gadrw
_wc_gadrw::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        ld      ix, #0
        add     ix, sp
        ld      d, 6(ix)                ; D = y
        ld      e, 7(ix)                ; E = x
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = win
        push    hl
        pop     ix                      ; IX = win
        ld      a, #0x05
        call    WC_ENTRY                ; HL = result addr
        pop     de                      ; remove saved win
        pop     ix                      ; restore caller IX
        ; callee cleans 2 bytes (y+x); preserve HL = return value
        pop     de                      ; DE = return address
        inc     sp
        inc     sp                      ; discard y(1)+x(1)
        push    de                      ; push return address back
        ret                             ; return, HL preserved

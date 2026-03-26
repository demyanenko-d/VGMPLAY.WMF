        .module wc_key_shift
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_shift
_wc_key_shift::
        push    ix
        ld      a, #0x28
        call    WC_ENTRY
        pop     ix
        ret

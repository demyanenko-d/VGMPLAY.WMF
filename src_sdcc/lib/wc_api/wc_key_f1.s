        .module wc_key_f1
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_f1
_wc_key_f1::
        push    ix
        ld      a, #0x1D
        call    WC_ENTRY
        pop     ix
        ret

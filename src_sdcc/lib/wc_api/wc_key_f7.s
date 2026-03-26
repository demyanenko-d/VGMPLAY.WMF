        .module wc_key_f7
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_f7
_wc_key_f7::
        push    ix
        ld      a, #0x23
        call    WC_ENTRY
        pop     ix
        ret

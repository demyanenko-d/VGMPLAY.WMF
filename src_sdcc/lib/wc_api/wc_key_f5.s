        .module wc_key_f5
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_f5
_wc_key_f5::
        push    ix
        ld      a, #0x21
        call    WC_ENTRY
        pop     ix
        ret

        .module wc_key_f8
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_f8
_wc_key_f8::
        push    ix
        ld      a, #0x24
        call    WC_ENTRY
        pop     ix
        ret

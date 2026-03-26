        .module wc_key_f4
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_f4
_wc_key_f4::
        push    ix
        ld      a, #0x20
        call    WC_ENTRY
        pop     ix
        ret

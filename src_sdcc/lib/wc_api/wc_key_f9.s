        .module wc_key_f9
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_f9
_wc_key_f9::
        push    ix
        ld      a, #0x25
        call    WC_ENTRY
        pop     ix
        ret

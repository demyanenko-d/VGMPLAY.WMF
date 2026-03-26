        .module wc_key_del
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_del
_wc_key_del::
        push    ix
        ld      a, #0x2B
        call    WC_ENTRY
        pop     ix
        ret

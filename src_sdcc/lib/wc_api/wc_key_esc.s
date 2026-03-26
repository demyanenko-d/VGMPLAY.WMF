        .module wc_key_esc
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_esc
_wc_key_esc::
        push    ix
        ld      a, #0x17
        call    WC_ENTRY
        pop     ix
        ret

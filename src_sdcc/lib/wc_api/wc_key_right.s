        .module wc_key_right
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_right
_wc_key_right::
        push    ix
        ld      a, #0x14
        call    WC_ENTRY
        pop     ix
        ret

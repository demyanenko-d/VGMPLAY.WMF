        .module wc_key_up
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_up
_wc_key_up::
        push    ix
        ld      a, #0x11
        call    WC_ENTRY
        pop     ix
        ret

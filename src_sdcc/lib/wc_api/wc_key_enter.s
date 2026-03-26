        .module wc_key_enter
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_enter
_wc_key_enter::
        push    ix
        ld      a, #0x16
        call    WC_ENTRY
        pop     ix
        ret

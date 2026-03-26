        .module wc_key_left
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_left
_wc_key_left::
        push    ix
        ld      a, #0x13
        call    WC_ENTRY
        pop     ix
        ret

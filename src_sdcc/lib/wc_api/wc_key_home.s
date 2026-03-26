        .module wc_key_home
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_home
_wc_key_home::
        push    ix
        ld      a, #0x1B
        call    WC_ENTRY
        pop     ix
        ret

        .module wc_key_space
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_space
_wc_key_space::
        push    ix
        ld      a, #0x10
        call    WC_ENTRY
        pop     ix
        ret

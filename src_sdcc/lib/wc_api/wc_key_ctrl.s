        .module wc_key_ctrl
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_ctrl
_wc_key_ctrl::
        push    ix
        ld      a, #0x29
        call    WC_ENTRY
        pop     ix
        ret

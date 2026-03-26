        .module wc_key_bspc
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_bspc
_wc_key_bspc::
        push    ix
        ld      a, #0x18
        call    WC_ENTRY
        pop     ix
        ret

        .module wc_key_f10
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_f10
_wc_key_f10::
        push    ix
        ld      a, #0x26
        call    WC_ENTRY
        pop     ix
        ret

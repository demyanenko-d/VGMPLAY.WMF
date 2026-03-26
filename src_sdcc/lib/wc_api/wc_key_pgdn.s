        .module wc_key_pgdn
        .area _CODE
WC_ENTRY = 0x6006
        .globl _wc_key_pgdn
_wc_key_pgdn::
        push    ix
        ld      a, #0x1A
        call    WC_ENTRY
        pop     ix
        ret

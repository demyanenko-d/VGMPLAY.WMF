; Matching continuation for opl4_loader.asm's page-switch trampoline.
; After OUT (#12AF),D at #BFFD switches Win2 from plugin page 5 back to page
; 0, the CPU fetches this RET at the same window's next address (#BFFF).
        .module cold_return
        .area _CABS (ABS)
        .org 0xBFFF
_opl4_page_return_gate::
        .db 0xC9

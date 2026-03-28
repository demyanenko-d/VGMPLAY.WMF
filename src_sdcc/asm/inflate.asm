; ============================================================================
; inflate.asm — TSConfig gzip inflate (adapted from SamFlate by Andrew Collier)
;
; Original SamFlate (C) 2009, released under DFSG-compatible license (BSD/GPL).
; TSConfig adaptation: replaces SAM Coupe LMPR/HMPR paging with TSConfig
; page-window port I/O (0x10AF–0x13AF).
;
; Memory layout during decompression (interrupts disabled):
;   Win 0 (0x0000–0x3FFF): Source VGZ data (rotating pages)
;   Win 1 (0x4000–0x7FFF): Source VGZ next page (contiguous 32K src window)
;                           Temporarily: backref history (dest_page - 1) during LDIR
;   Win 2 (0x8000–0xBFFF): Decode working buffer (fixed page PAGE_DECODE)
;   Win 3 (0xC000–0xFFFF): This code + RAM tables (extra WMF page)
;
; SamFlate was designed for SAM Coupe where LMPR gives a 32K contiguous
; source window (sections A+B).  On TSConfig, Win 0+Win 1 together provide
; the same 32K window so that source reads never fall off the page boundary
; between output flushes.  Win 1 is briefly remapped for backref LDIR, then
; immediately restored to src_page + 1.
;
; Entry: Called via JP 0xC200 (inflate_init) from inflate_call.s
;   A  = source start physical page (e.g. #A1 for TAP)
;   E  = destination start physical page (e.g. #20 for megabuffer)
;   D  = saved Win 2 page (to restore on exit)
;   SP = safe stack in Win 3 area (e.g. #FFF0)
;   Return address already pushed on stack by caller
;
; Exit: RET with A=0 carry clear (success) or A!=0 carry set (error).
;       Win 2 restored to saved page.  Caller restores Win 0/1/3.
;
; Assembled with sjasmplus -> raw binary, loaded as extra WMF page.
; NOTE: first 257 bytes at 0xC000 may be overwritten during inflate
;       (DEFLATE dict copy overflow).  Entry code runs once before inflate.
; ============================================================================

        DEVICE  NOSLOT64K
        ORG     #C000

; -- TSConfig port equates ---------------------------------------------------
PORT_W0         EQU     #10AF
PORT_W1         EQU     #11AF
PORT_W2         EQU     #12AF
PAGE_DECODE     EQU     #EA             ; free page for decode buffer

; -- Debug port equates (emulator shows 'Illegal port! OUT') ----------------
DBG_STATUS      EQU     #FFAF           ; status/milestone code
DBG_SRC         EQU     #FEAF           ; current src page
DBG_DST         EQU     #FDAF           ; current dst page

; -- Macro: emit debug milestone (destroys BC, A) ---------------------------
        MACRO   DBG_OUT val
    IFDEF DBG_ENABLE
        ld      a, val
        ld      bc, DBG_STATUS
        out     (c), a
    ENDIF
        ENDM

; -- Macro: emit debug milestone + src/dst pages (destroys BC, A) -----------
        MACRO   DBG_OUT_PAGES val
    IFDEF DBG_ENABLE
        ld      a, val
        ld      bc, DBG_STATUS
        out     (c), a
        ld      a, (src_page + 1)
        ld      bc, DBG_SRC
        out     (c), a
        ld      a, (dst_page + 1)
        ld      bc, DBG_DST
        out     (c), a
    ENDIF
        ENDM

; -- Macro: write A to TSConfig page window port (destroys BC) ---------------
        MACRO   OUT_W0
        ld      bc, PORT_W0
        out     (c), a
        ENDM

        MACRO   OUT_W1
        ld      bc, PORT_W1
        out     (c), a
        ENDM

        MACRO   OUT_W2
        ld      bc, PORT_W2
        out     (c), a
        ENDM

; -- Macro: unrolled Huffman decode body -------------------------------------
        MACRO   HUFFDEC lbl, mode, tgt
lbl:
        exx
        IF mode > 0
        dec     b
        or      a
        rr      c
        ELSE
        or      a
        rr      c
        ld      b, 8
        ld      c, (hl)
        inc     hl
        ENDIF
        exx
        adc     a, a
        add     a, a
        ld      l, a
        ld      a, (hl)
        bit     7, a
        jp      z, tgt
        inc     hl
        ld      h, (hl)
        ENDM


; ============================================================================
;  ENTRY POINT  (0xC000) -- JP to safe init zone
;
;  This 3-byte JP may be overwritten by DEFLATE backref LDIR overflow
;  (up to 258 bytes past 0xBFFF).  That's fine: by the time the overflow
;  happens, we've already jumped to init and are deep inside inflate_core.
;
;  inflate_call.s calls JP 0xC200 directly (inflate_init in safe zone).
;  The JP at 0xC000 is NOT used at runtime; it exists only as a
;  fallback marker and will be overwritten by LDIR on first call.
; ============================================================================
entry:
        jp      inflate_init

; ============================================================================
;  PADDING — survive DEFLATE backref overflow
;
;  During LDIR of a backref copy, DE can advance up to 258 bytes
;  past 0xBFFF into Win 3 (0xC000+), overwriting inflate code.
;  inflate_init MUST start at or after 0xC102 to survive.
;  We place it at 0xC200 for a clean round address.
; ============================================================================
        DS #C1F0 - $, 0

; ── Progress bar parameter block ────────────────────────────────────────
; Written by inflate_call.s before JP 0xC200.
; Safe from LDIR overflow (max 258 bytes → 0xC102 < 0xC1F0).
;
; inflate_call.s uses absolute addresses 0xC1F0–0xC1F7 to write these.
; ────────────────────────────────────────────────────────────────────────
pb_text_pg:     DB 0            ; 0xC1F0: text screen physical page
pb_attr_lo:     DB 0            ; 0xC1F1: attr base offset low byte (bit7=1, attr area)
pb_attr_hi:     DB 0            ; 0xC1F2: attr base offset high byte
pb_total:       DB 0            ; 0xC1F3: estimated total output pages (0=disabled)
pb_bar_w:       DB 0            ; 0xC1F4: bar width in columns
pb_green:       DB 0            ; 0xC1F5: green attr value
pb_error:       DB 0            ; 0xC1F6: Bresenham error accumulator (runtime)
pb_col:         DB 0            ; 0xC1F7: current filled column (runtime)

        ASSERT $ <= #C200, "progress block overflows past inflate_init!"
        DS #C200 - $, 0

; ============================================================================
;  INFLATE INIT — safe zone (≥ 0xC102), runs every call
;
;  Registers on entry:
;    A  = source start physical page
;    E  = destination start physical page
;    D  = saved Win 2 page (to restore on exit)
;    SP = safe stack in Win 3 area
;    Return address already pushed on stack
; ============================================================================
inflate_init:
        ASSERT $ == #C200, "inflate_init must be at exactly 0xC200 (inflate_call.s hardcodes this)"
        ; A = src start page, D = saved pg2, E = dst start page
        ld      (src_page + 1), a
        ld      a, d
        ld      (wc_saved_pg2 + 1), a
        ld      a, e
        ld      (dst_page + 1), a
        sub     2
        ld      (dst_page_m2 + 1), a

        ; Initial destination offset = 0
        ld      hl, 0
        ld      (dst + 1), hl

        ; Remap Win 2 = decode buffer page
        ld      a, PAGE_DECODE
        OUT_W2

        ; Remap Win 0+1 = first two source pages (32K contiguous window)
        ld      a, (src_page + 1)
        OUT_W0
        inc     a
        ld      b, HIGH PORT_W1         ; C still = #AF from OUT_W0
        out     (c), a                  ; Win 1 = src_page + 1

        ; Source pointer at start of Win 0, decode buffer at 0x8000
        ld      hl, #0000
        ld      de, #8000
        ; fall through to inflate_core

; ============================================================================
;  INFLATE CORE -- gzip header parse + DEFLATE blocks
; ============================================================================
inflate_core:
        ld      (interim_dst + 1), de

        ; HL'/C'/B' = source state (alternate registers)
        ld      c, (hl)
        inc     hl
        ld      b, 8
        exx

        ; -- gzip header --
        call    read8bits
        cp      #1F
        jp      nz, fail
        call    read8bits
        cp      #8B
        jp      nz, fail
        call    read8bits
        cp      8
        jp      nz, fail

        call    read8bits
        ld      d, a            ; flags
        ld      a, 6
        call    dropbytes

        ld      a, d
        and     #04
        call    nz, drop_gz_fextra
        ld      a, d
        and     #08
        call    nz, drop_gz_fname
        ld      a, d
        and     #10
        call    nz, drop_gz_fcomment
        ld      a, d
        and     #02
        call    nz, drop_gz_fhcrc


; -- DEFLATE block loop ------------------------------------------------------
        ; DBG: 0x12 = gzip header parsed OK, entering DEFLATE blocks
        DBG_OUT #12

NEW_BLOCK:
        call    read1bit
        ld      (this_was_last_block + 1), a
        call    read2bits
        cp      3
        jp      z, fail
        cp      1
        jp      c, blocktype0
        jp      z, blocktype1
        jp      blocktype2


; ============================================================================
;  BLOCK TYPE 0 -- stored (raw) data
; ============================================================================
blocktype0:
        DBG_OUT #14
        call    readalign
        call    read8bits
        ld      c, a
        call    read8bits
        ld      b, a
        call    read8bits
        ld      l, a
        call    read8bits
        ld      h, a
        or      a
        sbc     hl, bc
        ld      a, h
        or      l
        jp      nz, fail

        ld      de, (interim_dst + 1)

bt0_try_page:
        ld      l, e
        ld      h, d
        add     hl, bc
        ld      a, h
        sub     #80
        cp      #40
        jp      c, bt0_fits

        ; Won't fit: flush page
        ld      hl, #C000
        or      a
        sbc     hl, de
        ld      a, l
        ld      l, c
        ld      c, a
        ld      a, h
        ld      h, b
        ld      b, a
        or      a
        sbc     hl, bc
        ld      (bt0_remaining + 1), hl

        exx
        push    hl
        exx
        pop     hl
        ldir
        push    hl
        exx
        pop     hl
        exx

        ; Copy decode buffer -> destination
        ld      a, (dst_page + 1)
        OUT_W0

        ld      de, (dst + 1)
        ld      a, e
        or      d
        jr      z, bt0_whole

        ld      hl, #4000
        or      a
        sbc     hl, de
        ld      c, l
        ld      b, h
        ld      l, e
        ld      a, d
        or      #80
        ld      h, a
        ldir
        jr      bt0_flushed

bt0_whole:
        ld      hl, #8000
        ld      de, #0000
        ld      bc, #4000
        ldir

bt0_flushed:
        ld      hl, 0
        ld      (dst + 1), hl
        ld      a, (dst_page + 1)
        inc     a
        cp      #60                     ; megabuffer limit (0x20-0x5F)
        jp      nc, fail                ; overflow → abort
        ld      (dst_page + 1), a
        dec     a
        dec     a
        ld      (dst_page_m2 + 1), a

        ; DBG: 0x20 = bt0 page flushed
        DBG_OUT_PAGES #20

        ; Source rollover
        exx
        bit     6, h
        jr      z, bt0_no_srcroll
        ld      a, h
        and     #3F
        ld      h, a
        ld      a, (src_page + 1)
        inc     a
        ld      (src_page + 1), a
bt0_no_srcroll:
        exx

        ld      a, (src_page + 1)
        OUT_W0
        inc     a
        ld      b, HIGH PORT_W1         ; C still = #AF from OUT_W0
        out     (c), a                  ; Win 1 = src_page + 1
        call    progress_update

        ld      de, #8000
bt0_remaining   EQU     $
        ld      bc, #0000
        jp      bt0_try_page

bt0_fits:
        exx
        push    hl
        exx
        pop     hl
        ldir
        push    hl
        exx
        pop     hl
        exx
        ld      (interim_dst + 1), de
        jp      END_BLOCK


; ============================================================================
;  BLOCK TYPE 1 -- static Huffman
; ============================================================================
blocktype1:
        DBG_OUT #15
        ld      hl, 288
        ld      (literals + 1), hl
        ld      a, 32
        ld      (distances + 1), a

        ld      hl, codelengths
        ld      b, 144
        ld      a, 8
bt1_s1: ld      (hl), a
        inc     hl
        djnz    bt1_s1
        ld      b, 112
        ld      a, 9
bt1_s2: ld      (hl), a
        inc     hl
        djnz    bt1_s2
        ld      b, 24
        ld      a, 7
bt1_s3: ld      (hl), a
        inc     hl
        djnz    bt1_s3
        ld      b, 8
        ld      a, 8
bt1_s4: ld      (hl), a
        inc     hl
        djnz    bt1_s4
        jp      huffman_common


; ============================================================================
;  BLOCK TYPE 2 -- dynamic Huffman
; ============================================================================
blocktype2:
        DBG_OUT #16
        call    read5bits
        inc     a
        ld      l, a
        ld      h, 1
        ld      (literals + 1), hl

        call    read5bits
        inc     a
        ld      (distances + 1), a

        call    read4bits
        add     a, 4
        ld      (bt2_cll_cnt + 1), a

        ld      hl, codelengths
        ld      b, 20
        xor     a
bt2_clr:
        ld      (hl), a
        inc     hl
        djnz    bt2_clr

        ; DBG: 0xA6 = HLIT/HDIST/HCLEN read OK, reading code length orders
        DBG_OUT #A6

        ld      de, code_length_orders
        ld      h, HIGH codelengths
bt2_cll_cnt     EQU     $
        ld      b, 0
bt2_cll:
        ld      a, (de)
        add     a, LOW codelengths
        ld      l, a
        call    read3bits
        ld      (hl), a
        inc     de
        djnz    bt2_cll

        ; DBG: 0xA9 = code length orders read OK, calling makeHuffmanTable
        DBG_OUT #A9

        ld      bc, 19
        ld      hl, codelengths
        ld      de, Huffman_Tables_Start
        call    makeHuffmanTable

        ; DBG: 0xA7 = first makeHuffmanTable done (proto codes)
        DBG_OUT #A7

        ld      hl, Huffman_Tables_Start
        ld      de, codelengths
literals        EQU     $
        ld      bc, #0000
        ld      a, (distances + 1)
        add     a, c
        ld      c, a
        ld      a, b
        adc     a, 0
        ld      b, a
        call    decodeProtoHuffman

        ; DBG: 0xA8 = decodeProtoHuffman done
        DBG_OUT #A8

; -- common Huffman table building -------------------------------------------
huffman_common:
        ld      bc, (literals + 1)
        ld      hl, codelengths
        ld      de, Huffman_Tables_Start
        call    makeHuffmanTable

        ld      de, (next_htable_store + 1)
        ld      (Dist_HT_Start + 1), de

        ld      hl, codelengths
        ld      bc, (literals + 1)
        add     hl, bc
distances       EQU     $
        ld      c, 0
        ld      b, 0
        call    makeHuffmanTable

        ; Set up decode loop
interim_dst     EQU     $
        ld      de, #8000

Dist_HT_Start  EQU     $
        ld      hl, #0000
        ld      a, l
        rra
        rra
        ld      l, a
        ld      (Dist_HTBegin + 1), hl

        ld      hl, Huffman_Tables_Start
        ld      a, l
        rra
        rra
        ld      l, a
        ld      (HTBegin + 1), hl


        ; DBG: 0x17 = Huffman tables built, entering decode loop
        DBG_OUT #17

; ============================================================================
;  HUFFMAN DECODE LOOP -- unrolled 8x
; ============================================================================
decode_tableloop:
        exx
        ld      a, b
        exx
        add     a, a
        add     a, a
        ld      (dtl_jr + 1), a

HTBegin         EQU     $
        ld      hl, #0000
        ld      a, l

dtl_jr          EQU     $
        jr      $+2
        jp      fail
        nop
        jp      decode_tlu_0
        nop
        jp      decode_tlu_7
        nop
        jp      decode_tlu_6
        nop
        jp      decode_tlu_5
        nop
        jp      decode_tlu_4
        nop
        jp      decode_tlu_3
        nop
        jp      decode_tlu_2
        nop
        jp      decode_tlu_1

        HUFFDEC decode_tlu_0, 0, decoded_symbol
        HUFFDEC decode_tlu_1, 1, decoded_symbol
        HUFFDEC decode_tlu_2, 1, decoded_symbol
        HUFFDEC decode_tlu_3, 1, decoded_symbol
        HUFFDEC decode_tlu_4, 1, decoded_symbol
        HUFFDEC decode_tlu_5, 1, decoded_symbol
        HUFFDEC decode_tlu_6, 1, decoded_symbol
        HUFFDEC decode_tlu_7, 1, decoded_symbol
        jp      decode_tlu_0


decoded_symbol:
        or      a
        jr      nz, decode_special

        inc     hl
        ld      a, (hl)
        ldi

        bit     6, d
        jp      nz, copy_decoded_page
        jp      decode_tableloop


end_of_huffman_block:
        ld      (interim_dst + 1), de
        jp      END_BLOCK


; -- Length/distance decode ---------------------------------------------------
decode_special:
        inc     hl
        ld      a, (hl)
        or      a
        jr      z, end_of_huffman_block

        ld      hl, lengthcodetable - 4
        add     a, a
        add     a, a
        add     a, l
        ld      l, a
        ld      a, h
        adc     a, 0
        ld      h, a

        ld      c, (hl)
        inc     hl
        ld      b, (hl)
        inc     hl
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        ld      (len_call + 1), hl

len_call        EQU     $
        call    #0000
        add     a, c
        ld      c, a
        ld      a, b
        adc     a, 0
        ld      b, a


; -- Distance decode loop ----------------------------------------------------
dist_decode_tl:
        exx
        ld      a, b
        exx
        add     a, a
        add     a, a
        ld      (ddtl_jr + 1), a

Dist_HTBegin    EQU     $
        ld      hl, #0000
        ld      a, l

ddtl_jr         EQU     $
        jr      $+2
        jp      fail
        nop
        jp      dist_tlu_0
        nop
        jp      dist_tlu_7
        nop
        jp      dist_tlu_6
        nop
        jp      dist_tlu_5
        nop
        jp      dist_tlu_4
        nop
        jp      dist_tlu_3
        nop
        jp      dist_tlu_2
        nop
        jp      dist_tlu_1

        HUFFDEC dist_tlu_0, 0, dist_sym
        HUFFDEC dist_tlu_1, 1, dist_sym
        HUFFDEC dist_tlu_2, 1, dist_sym
        HUFFDEC dist_tlu_3, 1, dist_sym
        HUFFDEC dist_tlu_4, 1, dist_sym
        HUFFDEC dist_tlu_5, 1, dist_sym
        HUFFDEC dist_tlu_6, 1, dist_sym
        HUFFDEC dist_tlu_7, 1, dist_sym
        jp      dist_tlu_0

dist_sym:
        inc     hl
        ld      a, (hl)
        add     a, a
        ld      l, a
        add     a, a
        add     a, l
        ld      hl, distancecodetable
        add     a, l
        ld      l, a
        ld      a, h
        adc     a, 0
        ld      h, a

        push    de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        inc     hl
        push    de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        inc     hl
        ld      (dcall1 + 1), de
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        ld      (dcall2 + 1), de

dcall1          EQU     $
        call    #0000
        ld      e, a
dcall2          EQU     $
        call    #0000
        ld      d, a

        pop     hl
        add     hl, de
        ex      de, hl
        ld      hl, 0
        or      a
        sbc     hl, de
        pop     de
        add     hl, de          ; HL = write_ptr - distance


; ============================================================================
;  BACKREF COPY -- TSConfig paging for history access
; ============================================================================
        push    bc
dst_page_m2     EQU     $
        ld      a, 0
        ld      bc, PORT_W0
        out     (c), a          ; Win 0 = dest_page - 2
        inc     a
        ld      b, HIGH PORT_W1
        out     (c), a          ; Win 1 = dest_page - 1
        pop     bc

        ldir

        ; Restore Win 0+1 = source pages (BC=0 after LDIR)
src_page        EQU     $
        ld      a, 0
        OUT_W0
        inc     a
        ld      b, HIGH PORT_W1         ; C still = #AF from OUT_W0
        out     (c), a                  ; Win 1 = src_page + 1

        bit     6, d
        jp      z, decode_tableloop
        ; fall through


; ============================================================================
;  FLUSH DECODE BUFFER to destination page
; ============================================================================
copy_decoded_page:
        ld      (cpd_extra + 1), de

dst_page        EQU     $
        ld      a, 0
        OUT_W0

dst             EQU     $
        ld      de, 0
        ld      a, e
        or      d
        jr      z, cpd_whole

        ; Partial page
        ld      hl, #4000
        or      a
        sbc     hl, de
        ld      c, l
        ld      b, h
        ld      l, e
        ld      a, d
        or      #80
        ld      h, a
        ldir
        jr      cpd_done

cpd_whole:
        ld      hl, #8000
        ld      de, #0000
        ld      bc, #4000
        ldir

cpd_done:
        ld      hl, 0
        ld      (dst + 1), hl
        ld      a, (dst_page + 1)
        inc     a
        cp      #60                     ; megabuffer limit (0x20-0x5F)
        jp      nc, fail                ; overflow → abort
        ld      (dst_page + 1), a
        dec     a
        dec     a
        ld      (dst_page_m2 + 1), a

        ; DBG: 0x21 = cpd page flushed
        DBG_OUT_PAGES #21

        ld      de, #8000

cpd_extra       EQU     $
        ld      bc, #0000
        ld      a, b
        and     #3F
        ld      b, a
        or      c
        jr      z, cpd_no_extra
        ld      hl, #C000
        ldir

cpd_no_extra:
        ; Source rollover
        exx
        bit     6, h
        jr      z, cpd_no_srcroll
        ld      a, h
        and     #3F
        ld      h, a
        ld      a, (src_page + 1)
        inc     a
        ld      (src_page + 1), a
cpd_no_srcroll:
        exx
        ld      a, (src_page + 1)
        OUT_W0
        inc     a
        ld      b, HIGH PORT_W1         ; C still = #AF from OUT_W0
        out     (c), a                  ; Win 1 = src_page + 1
        push    de                      ; save decode write ptr (DE clobbered by progress_update)
        call    progress_update
        pop     de                      ; restore decode write ptr
        jp      decode_tableloop


; ============================================================================
;  END_BLOCK -- last block check, final flush, exit
; ============================================================================
END_BLOCK:
this_was_last_block     EQU     $
        ld      a, 0
        or      a
        jp      z, NEW_BLOCK

        ; Final flush
        ld      a, (dst_page + 1)
        OUT_W0
        ld      de, (dst + 1)
        ld      hl, (interim_dst + 1)
        or      a
        sbc     hl, de
        ld      c, l
        ld      a, h
        and     #3F
        ld      b, a
        or      c
        jr      z, exit_ok
        ld      l, e
        ld      a, d
        and     #3F
        ld      d, a
        or      #80
        ld      h, a
        ldir

exit_ok:
        ; DBG: 0x30 = exit_ok
        DBG_OUT_PAGES #30

wc_saved_pg2    EQU     $
        ld      a, 0
        OUT_W2
        xor     a
        ret

fail:
        ; DBG: 0x31 = fail
        DBG_OUT #31

        ld      a, (wc_saved_pg2 + 1)
        OUT_W2
        ld      a, 1
        scf
        ret


; ============================================================================
;  PROGRESS BAR UPDATE (Bresenham proportional column fill)
;
;  Called after each output page flush (from copy_decoded_page and bt0 flush).
;  Briefly remaps Win 0 to the text screen page, writes green attr bytes
;  to fill the progress bar proportionally, restores Win 0 to source page.
;
;  Destroys: A, BC, DE, HL.  Alternate registers untouched.
;  Parameters in pb_* block at 0xC1F0 (set by inflate_call.s before entry).
; ============================================================================
progress_update:
        ld      a, (pb_total)
        or      a
        ret     z                       ; 0 = disabled

        ; Bar already full?
        ld      a, (pb_col)
        ld      hl, pb_bar_w
        cp      (hl)
        ret     nc                      ; col >= width → done

        ; Bresenham: error += bar_width
        ld      a, (pb_error)
        add     a, (hl)                 ; (hl) still = pb_bar_w
        ld      d, a                    ; D = new error

        ; E = total
        ld      a, (pb_total)
        ld      e, a

        ; error < total → no column this time
        ld      a, d
        cp      e
        jr      c, pb_save_err          ; skip fill, just save error

        ; ═══ At least one column to fill ═══
        ; Map Win 0 → text screen page
        ld      a, (pb_text_pg)
        OUT_W0                          ; destroys BC

        ; HL = attr base address (bit7 of L is already set)
        ld      a, (pb_attr_hi)
        ld      h, a
        ld      a, (pb_attr_lo)
        ld      l, a
        ld      a, (pb_col)
        add     a, l
        ld      l, a                    ; attr area: stride 1, no overflow
        ; C = green attr value, B = bar width limit
        ld      a, (pb_green)
        ld      c, a
        ld      a, (pb_bar_w)
        ld      b, a

pb_fill_loop:
        ld      (hl), c                 ; write green attr
        inc     l                       ; next column (stride 1, same row)

        ; pb_col++
        ld      a, (pb_col)
        inc     a
        ld      (pb_col), a
        cp      b                       ; col >= bar_width?
        jr      nc, pb_fill_done

        ; error -= total
        ld      a, d
        sub     e
        ld      d, a
        cp      e                       ; still >= total?
        jr      nc, pb_fill_loop

pb_fill_done:
        ; Restore Win 0 → source page
        ld      a, (src_page + 1)
        OUT_W0                          ; destroys BC

pb_save_err:
        ld      a, d
        ld      (pb_error), a
        ret


; ============================================================================
;  GZIP HEADER SKIP ROUTINES
; ============================================================================
drop_gz_fextra:
        push    hl
        push    bc
        call    read8bits
        ld      l, a
        call    read8bits
        ld      h, a
        or      c
        jp      z, fextra_ret
        ld      bc, 255
fextra_lp:
        ld      a, h
        or      a
        jr      z, fextra_last
        ld      a, 255
        call    dropbytes
        or      a
        sbc     hl, bc
        jr      fextra_lp
fextra_last:
        ld      a, l
        call    dropbytes
fextra_ret:
        pop     bc
        pop     hl
        ret

drop_gz_fname:
drop_gz_fcomment:
        call    read8bits
        or      a
        jr      nz, drop_gz_fname
        ret

drop_gz_fhcrc:
        ld      a, 2
        call    dropbytes
        ret


; ============================================================================
;  UTILITY
; ============================================================================
dropbytes:
        or      a
        ret     z
        push    bc
        ld      b, a
db_lp:  call    read8bits
        djnz    db_lp
        pop     bc
        ret


; ============================================================================
;  makeHuffmanTable
; ============================================================================
makeHuffmanTable:
        ld      (mht_nsym + 1), bc
        ld      (mht_clens + 1), hl
        ld      (mht_tstore + 1), de

        ld      hl, codelength_count
        ld      b, 18
mht_clr:
        ld      (hl), 0
        inc     hl
        ld      (hl), 0
        inc     hl
        djnz    mht_clr

mht_nsym        EQU     $
        ld      bc, #0000
mht_clens       EQU     $
        ld      hl, #0000
        ld      de, codelength_count

mht_cnt:
        ld      a, (hl)
        inc     hl
        add     a, a
        ld      e, a
        ld      a, (de)
        inc     a
        ld      (de), a
        or      a
        jr      nz, mht_cnt_ok
        inc     de
        ld      a, (de)
        inc     a
        ld      (de), a
mht_cnt_ok:
        dec     bc
        ld      a, b
        or      c
        jr      nz, mht_cnt

        ; DBG: 0xB1 = mht_cnt done (count loop)
        DBG_OUT #B1

        xor     a
        ld      e, a
        ld      (de), a
        inc     de
        ld      (de), a

        ld      e, a
        ld      d, a
        ld      c, a
        ld      b, 1
mht_sc:
        ld      h, HIGH codelength_count
        ld      a, b
        dec     a
        add     a, a
        ld      l, a
        ld      a, (hl)
        add     a, e
        ld      e, a
        inc     hl
        ld      a, (hl)
        adc     a, d
        ld      d, a
        ld      a, c
        adc     a, 0
        ld      c, a
        rl      e
        rl      d
        rl      c
        ld      h, HIGH next_code
        ld      a, b
        rla
        rla
        ld      l, a
        ld      (hl), e
        inc     hl
        ld      (hl), d
        inc     hl
        ld      (hl), c
        inc     hl
        ld      (hl), 0
        inc     b
        ld      a, b
        cp      19
        jr      c, mht_sc

        ; DBG: 0xB2 = mht_sc done (start codes)
        DBG_OUT #B2

        ld      bc, 0
mht_assign:
        ld      hl, (mht_clens + 1)
        add     hl, bc
        ld      a, (hl)
        or      a
        jr      z, mht_nocode

        rla
        rla
        ld      l, a
        ld      h, HIGH next_code
        push    hl
        ld      e, (hl)
        inc     hl
        ld      d, (hl)
        inc     hl
        ld      a, (hl)

        ld      hl, codes
        add     hl, bc
        add     hl, bc
        add     hl, bc
        add     hl, bc
        ld      (hl), e
        inc     hl
        ld      (hl), d
        inc     hl
        ld      (hl), a

        pop     hl
        inc     e
        ld      (hl), e
        inc     hl
        ld      e, a
        ld      a, d
        adc     a, 0
        ld      (hl), a
        inc     hl
        ld      a, e
        adc     a, 0
        ld      (hl), a

mht_nocode:
        inc     bc
        ld      hl, (mht_nsym + 1)
        or      a
        sbc     hl, bc
        jr      nz, mht_assign

        ; DBG: 0xB3 = mht_assign done (code assignment)
        DBG_OUT #B3

        ; Build lookup tables
        ld      hl, (mht_tstore + 1)
        xor     a
        ld      (hl), a
        inc     hl
        ld      (hl), a
        inc     hl
        ld      (hl), a
        inc     hl
        ld      (hl), a
        inc     hl
        ld      (next_htable_store + 1), hl

        ld      hl, 0
        ld      (mht_tabulated + 1), hl
        ld      de, (mht_tabulated + 1)

mht_predec:
        ld      hl, (mht_clens + 1)
        add     hl, de
        ld      a, (hl)
        or      a
        jp      z, mht_unused
        ld      b, a

        ld      hl, codes
        add     hl, de
        add     hl, de
        add     hl, de
        add     hl, de
        ld      d, (hl)
        inc     hl
        ld      c, (hl)
        inc     hl
        ld      a, (hl)
        rra
        rr      c
        rr      d
        rra
        rr      c
        rr      d
        rra
        ld      e, a

        ld      a, 18
        sub     b
        jr      z, mht_nodiscard
        or      a
mht_discard:
        rl      e
        rl      d
        rl      c
        dec     a
        jr      nz, mht_discard
mht_nodiscard:

mht_tstore      EQU     $
        ld      hl, #0000

mht_nextbit:
        rl      e
        rl      d
        rl      c
        jr      nc, mht_bitzero
        inc     hl
        inc     hl
mht_bitzero:
        dec     b
        jr      z, mht_wrsym

        ld      a, (hl)
        or      a
        jr      z, mht_wrtable

        inc     hl
        ld      h, (hl)
        add     a, a
        add     a, a
        ld      l, a
        jr      mht_nextbit

mht_wrtable:
        push    de
next_htable_store       EQU     $
        ld      de, #0000
        ld      a, e
        rra
        scf
        rra
        ld      (hl), a
        inc     hl
        ld      (hl), d
        ld      l, e
        ld      h, d
        xor     a
        ld      (de), a
        inc     de
        ld      (de), a
        inc     de
        ld      (de), a
        inc     de
        ld      (de), a
        inc     de
        ld      (next_htable_store + 1), de
        pop     de
        jr      mht_nextbit

mht_wrsym:
mht_tabulated   EQU     $
        ld      de, #0000
        ld      (hl), d
        inc     hl
        ld      (hl), e

mht_unused:
        inc     de
        ld      (mht_tabulated + 1), de
        ld      hl, (mht_nsym + 1)
        or      a
        sbc     hl, de
        ld      a, h
        or      l
        jp      nz, mht_predec
        ret


; ============================================================================
;  decodeProtoHuffman
; ============================================================================
decodeProtoHuffman:
        ld      a, l
        rra
        rra
        ld      l, a
        ld      (dph_begin + 1), hl
        IFDEF DBG_ENABLE
        ; DBG: 0xBF = decodeProtoHuffman entry
        push    af
        push    bc
        ld      a, #BF
        ld      bc, DBG_STATUS
        out     (c), a
        pop     bc
        pop     af
        ENDIF
        jr      dph_jump

dph_tloop:
        inc     hl
        ld      h, (hl)
        ld      l, a
dph_jump:
        call    read1bit
        rra
        ld      a, l
        adc     a, a
        add     a, a
        ld      l, a
        ld      a, (hl)
        bit     7, a
        jr      nz, dph_tloop
        inc     hl
        ld      a, (hl)
        IFDEF DBG_ENABLE
        ; DBG: 0xC1 = leaf found, FEAF = symbol value
        push    af
        push    bc
        ld      bc, DBG_SRC
        out     (c), a
        ld      a, #C1
        ld      bc, DBG_STATUS
        out     (c), a
        pop     bc
        pop     af
        ENDIF
        cp      16
        jr      nc, dph_special
        ld      (de), a
        inc     de
        dec     bc
dph_loop:
        IFDEF DBG_ENABLE
        ; DBG: 0xC0 = dph loop iteration
        push    af
        push    bc
        ld      a, #C0
        ld      bc, DBG_STATUS
        out     (c), a
        pop     bc
        pop     af
        ENDIF
dph_begin       EQU     $
        ld      hl, #0000
        ld      a, b
        or      c
        jr      nz, dph_jump
        ret

dph_special:
        IFDEF DBG_ENABLE
        ; DBG: 0xC2 = special code, FEAF = code value
        push    af
        push    bc
        ld      bc, DBG_SRC
        out     (c), a
        ld      a, #C2
        ld      bc, DBG_STATUS
        out     (c), a
        pop     bc
        pop     af
        ENDIF
        cp      17
        jr      z, dph_17
        jr      nc, dph_18
        ; proto 16
        call    read2bits
        add     a, 3
        ld      h, a
        dec     de
        ld      a, (de)
        inc     de
dph_p16:
        ld      (de), a
        inc     de
        dec     bc
        dec     h
        jr      nz, dph_p16
        jr      dph_loop

dph_17:
        call    read3bits
        add     a, 3
        ld      h, a
        xor     a
dph_p17:
        ld      (de), a
        inc     de
        dec     bc
        dec     h
        jr      nz, dph_p17
        jr      dph_loop

dph_18:
        call    read7bits
        add     a, 11
        ld      h, a
        xor     a
dph_p18:
        ld      (de), a
        inc     de
        dec     bc
        dec     h
        jr      nz, dph_p18
        jr      dph_loop


; ============================================================================
;  BIT READ FUNCTIONS
;  Alternate regs: HL'=src ptr, C'=current byte, B'=valid bits
; ============================================================================

read0bits:
        xor     a
        ret

read1bit:
read1bits:
        exx
        dec     b
        jr      z, r1_refill
        xor     a
        rr      c
        rla
        exx
        ret
r1_refill:
        xor     a
        rr      c
        rla
        ld      b, 8
        ld      c, (hl)
        inc     hl
        exx
        ret

; -- read2bits ---------------------------------------------------------------
read2bits:
        exx
        ld      a, b
        sub     2
        jr      c, r2_need
        jr      z, r2_exact
        ld      b, a
        ld      e, c
        ld      a, c
        rra
        rra
        and     #3F
        ld      c, a
        ld      a, e
        and     #03
        exx
        ret
r2_exact:
        ld      a, c
        ld      b, 8
        ld      c, (hl)
        inc     hl
        exx
        ret
r2_need:
        ld      e, c
        ld      d, b
        ld      c, (hl)
        inc     hl
        ld      a, c
r2_sl:  add     a, a
        djnz    r2_sl
        or      e
        and     #03
        ld      e, a
        ld      a, 2
        sub     d
        ld      b, a
        ld      a, c
r2_sr:  or      a
        rra
        djnz    r2_sr
        ld      c, a
        ld      a, d
        sub     2
        add     a, 8
        ld      b, a
        ld      a, e
        exx
        ret

; -- read3bits ---------------------------------------------------------------
read3bits:
        exx
        ld      a, b
        sub     3
        jr      c, r3_need
        jr      z, r3_exact
        ld      b, a
        ld      e, c
        ld      a, c
        rra
        rra
        rra
        and     #1F
        ld      c, a
        ld      a, e
        and     #07
        exx
        ret
r3_exact:
        ld      a, c
        ld      b, 8
        ld      c, (hl)
        inc     hl
        exx
        ret
r3_need:
        ld      e, c
        ld      d, b
        ld      c, (hl)
        inc     hl
        ld      a, c
r3_sl:  add     a, a
        djnz    r3_sl
        or      e
        and     #07
        ld      e, a
        ld      a, 3
        sub     d
        ld      b, a
        ld      a, c
r3_sr:  or      a
        rra
        djnz    r3_sr
        ld      c, a
        ld      a, d
        sub     3
        add     a, 8
        ld      b, a
        ld      a, e
        exx
        ret

; -- read4bits ---------------------------------------------------------------
read4bits:
        exx
        ld      a, b
        sub     4
        jr      c, r4_need
        jr      z, r4_exact
        ld      b, a
        ld      e, c
        ld      a, c
        rra
        rra
        rra
        rra
        and     #0F
        ld      c, a
        ld      a, e
        and     #0F
        exx
        ret
r4_exact:
        ld      a, c
        ld      b, 8
        ld      c, (hl)
        inc     hl
        exx
        ret
r4_need:
        ld      e, c
        ld      d, b
        ld      c, (hl)
        inc     hl
        ld      a, c
r4_sl:  add     a, a
        djnz    r4_sl
        or      e
        and     #0F
        ld      e, a
        ld      a, 4
        sub     d
        ld      b, a
        ld      a, c
r4_sr:  or      a
        rra
        djnz    r4_sr
        ld      c, a
        ld      a, d
        sub     4
        add     a, 8
        ld      b, a
        ld      a, e
        exx
        ret

; -- read5bits ---------------------------------------------------------------
read5bits:
        exx
        ld      a, b
        sub     5
        jr      c, r5_need
        jr      z, r5_exact
        ld      b, a
        ld      e, c
        ld      a, c
        rlca
        rlca
        rlca
        and     #07
        ld      c, a
        ld      a, e
        and     #1F
        exx
        ret
r5_exact:
        ld      a, c
        ld      b, 8
        ld      c, (hl)
        inc     hl
        exx
        ret
r5_need:
        ld      e, c
        ld      d, b
        ld      c, (hl)
        inc     hl
        ld      a, c
r5_sl:  add     a, a
        djnz    r5_sl
        or      e
        and     #1F
        ld      e, a
        ld      a, 5
        sub     d
        ld      b, a
        ld      a, c
r5_sr:  or      a
        rra
        djnz    r5_sr
        ld      c, a
        ld      a, d
        sub     5
        add     a, 8
        ld      b, a
        ld      a, e
        exx
        ret

; -- read6bits ---------------------------------------------------------------
read6bits:
        exx
        ld      a, b
        sub     6
        jr      c, r6_need
        jr      z, r6_exact
        ld      b, a
        ld      e, c
        ld      a, c
        rlca
        rlca
        and     #03
        ld      c, a
        ld      a, e
        and     #3F
        exx
        ret
r6_exact:
        ld      a, c
        ld      b, 8
        ld      c, (hl)
        inc     hl
        exx
        ret
r6_need:
        ld      e, c
        ld      d, b
        ld      c, (hl)
        inc     hl
        ld      a, c
r6_sl:  add     a, a
        djnz    r6_sl
        or      e
        and     #3F
        ld      e, a
        ld      a, 6
        sub     d
        ld      b, a
        ld      a, c
r6_sr:  or      a
        rra
        djnz    r6_sr
        ld      c, a
        ld      a, d
        sub     6
        add     a, 8
        ld      b, a
        ld      a, e
        exx
        ret

; -- read7bits ---------------------------------------------------------------
read7bits:
        exx
        ld      a, b
        sub     7
        jr      c, r7_need
        jr      z, r7_exact
        ld      b, a
        ld      e, c
        ld      a, c
        rlca
        and     #01
        ld      c, a
        ld      a, e
        and     #7F
        exx
        ret
r7_exact:
        ld      a, c
        ld      b, 8
        ld      c, (hl)
        inc     hl
        exx
        ret
r7_need:
        ld      e, c
        ld      d, b
        ld      c, (hl)
        inc     hl
        ld      a, c
r7_sl:  add     a, a
        djnz    r7_sl
        or      e
        and     #7F
        ld      e, a
        ld      a, 7
        sub     d
        ld      b, a
        ld      a, c
r7_sr:  or      a
        rra
        djnz    r7_sr
        ld      c, a
        ld      a, d
        sub     7
        add     a, 8
        ld      b, a
        ld      a, e
        exx
        ret

; -- read8bits ---------------------------------------------------------------
read8bits:
        exx
        ld      a, b
        sub     8
        jr      c, r8_need
        ; b was 8 (exact)
        ld      a, c
        ld      b, 8
        ld      c, (hl)
        inc     hl
        exx
        ret
r8_need:
        ld      e, c
        ld      d, b
        ld      c, (hl)
        inc     hl
        ld      a, c
r8_sl:  add     a, a
        djnz    r8_sl
        or      e
        ld      e, a
        ld      a, 8
        sub     d
        ld      b, a
        ld      a, c
r8_sr:  or      a
        rra
        djnz    r8_sr
        ld      c, a
        ld      a, d
        sub     8
        add     a, 8
        ld      b, a
        ld      a, e
        exx
        ret

; Byte-align
readalign:
        exx
        ld      c, (hl)
        inc     hl
        ld      b, 8
        exx
        ret


; ============================================================================
;  DATA TABLES
; ============================================================================

lengthcodetable:
        DW      3, read0bits
        DW      4, read0bits
        DW      5, read0bits
        DW      6, read0bits
        DW      7, read0bits
        DW      8, read0bits
        DW      9, read0bits
        DW      10, read0bits
        DW      11, read1bits
        DW      13, read1bits
        DW      15, read1bits
        DW      17, read1bits
        DW      19, read2bits
        DW      23, read2bits
        DW      27, read2bits
        DW      31, read2bits
        DW      35, read3bits
        DW      43, read3bits
        DW      51, read3bits
        DW      59, read3bits
        DW      67, read4bits
        DW      83, read4bits
        DW      99, read4bits
        DW      115, read4bits
        DW      131, read5bits
        DW      163, read5bits
        DW      195, read5bits
        DW      227, read5bits
        DW      258, read0bits

distancecodetable:
        DW      1, read0bits, read0bits
        DW      2, read0bits, read0bits
        DW      3, read0bits, read0bits
        DW      4, read0bits, read0bits
        DW      5, read1bits, read0bits
        DW      7, read1bits, read0bits
        DW      9, read2bits, read0bits
        DW      13, read2bits, read0bits
        DW      17, read3bits, read0bits
        DW      25, read3bits, read0bits
        DW      33, read4bits, read0bits
        DW      49, read4bits, read0bits
        DW      65, read5bits, read0bits
        DW      97, read5bits, read0bits
        DW      129, read6bits, read0bits
        DW      193, read6bits, read0bits
        DW      257, read7bits, read0bits
        DW      385, read7bits, read0bits
        DW      513, read8bits, read0bits
        DW      769, read8bits, read0bits
        DW      1025, read8bits, read1bits
        DW      1537, read8bits, read1bits
        DW      2049, read8bits, read2bits
        DW      3073, read8bits, read2bits
        DW      4097, read8bits, read3bits
        DW      6145, read8bits, read3bits
        DW      8193, read8bits, read4bits
        DW      12289, read8bits, read4bits
        DW      16385, read8bits, read5bits
        DW      24577, read8bits, read5bits

code_length_orders:
        DB      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15

        DB      "tsconf-inflate", 0

        DISPLAY "Code size: ", /D, $ - #C000, " bytes"


; ============================================================================
;  RAM TABLES (zero-initialized)
; ============================================================================

        ALIGN   256
codelength_count:
        DS      36

        ALIGN   32
codelengths:
        DS      288 + 33

        ALIGN   256
next_code:
        DS      128

        ALIGN   256
codes:
        DS      288 * 4

        ALIGN   4
Huffman_Tables_Start:

        DISPLAY "Huffman space: ", /D, #FFFE - Huffman_Tables_Start + 2, " bytes"

        SAVEBIN "build/inflate.bin", #C000, $ - #C000

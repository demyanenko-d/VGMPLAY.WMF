;==============================================================================
; spectrum.s — Spectrum analyzer: render + hot-loop helpers (Z80 ASM)
;
; Hand-optimised replacements for C functions in main.c / vgm.c:
;   spectrum_render()      — 4-row bar graph, direct VRAM write
;   spectrum_opl_b0(r,v)   — OPL B0-B8 key-on + 0xBD percussion
;   spectrum_opl_b1(r,v)   — OPL Bank1 B0-B6 key-on
;   spectrum_ay(r,v,chip2) — AY period shadow + vol trigger + YM2203 FM
;   spectrum_saa(r,v)      — SAA octave shadow + amp trigger
;
; sdcccall(1) conventions:
;   1st u8 → A,  2nd u8 → L,  3rd u8 → stack+2
;   1st u16 → HL, return u8 → A, return u16 → HL
;
; Port 0x13AF = Page3 window (TSConfig)
;==============================================================================

        .module spectrum

; ── Exports ──────────────────────────────────────────────────────────
        .globl  _spectrum_render
        .globl  _spectrum_opl_b0
        .globl  _spectrum_opl_b1
        .globl  _spectrum_ay
        .globl  _spectrum_saa
        .globl  _spec_period_to_bar

; ── Imports ──────────────────────────────────────────────────────────
        .globl  _spectrum_levels        ; uint8_t[16] in isr.s _DATA
        .globl  _s_pb_text_pg           ; uint8_t in main.c _DATA
        .globl  _s_spec_char_ofs        ; uint16_t[4] in main.c _DATA
        .globl  _s_spec_attr_ofs        ; uint16_t[4] in main.c _DATA
        .globl  _spec_mask              ; uint16_t in vgm.c _DATA
        .globl  _spec_ay_period         ; uint16_t[6] in vgm.c _DATA
        .globl  _spec_saa_oct           ; uint8_t[6] in vgm.c _DATA
        .globl  _spec_opl_bd            ; uint8_t in vgm.c _DATA
        .globl  _spec_fm_block          ; uint8_t[6] in vgm.c _DATA

; ── Constants ────────────────────────────────────────────────────────
PORT_PAGE3   = 0x13AF
SPEC_BARS    = 16
SPEC_LEFT_PAD = 5
SPEC_BAR_W   = 3        ; 2 filled + 1 space
CHAR_FULL    = 0xDB      ; █
CHAR_HALF    = 0xDC      ; ▄
CLR_WIN      = 0x70      ; WC_COLOR(WC_WHITE, WC_BLACK)

;======================================================================
; Scratch variables in _DATA (kept minimal)
;======================================================================
        .area   _DATA

say_base:       .db 0                   ; temp: chip base index (0 or 3)
sr_row_ctr:     .db 0                   ; temp: row iteration counter

        .area   _CODE

; Row parameter table: [thresh_full, thresh_half, color] × 4 rows
; Row 0 (top=peak): thresholds 8,7  color=BRIGHT_RED
; Row 1: thresholds 6,5  color=BRIGHT_YELLOW
; Row 2: thresholds 4,3  color=BRIGHT_GREEN
; Row 3 (bottom): thresholds 2,1  color=GREEN
sr_row_params:
        .db     8, 7, 0x0A             ; row 0: bright red
        .db     6, 5, 0x0E             ; row 1: bright yellow
        .db     4, 3, 0x0C             ; row 2: bright green
        .db     2, 1, 0x04             ; row 3: green

;======================================================================
; spectrum_render(void)
;
; Assumptions:
;   - _s_spec_char_ofs and _s_spec_attr_ofs are arrays of 16-bit offsets
;   - _spectrum_levels is an array of SPECTRUM_BARS bytes
;   - PORT_PAGE3 is the page-switch I/O port
;   - SPEC_CHAR_FULL / SPEC_CHAR_HALF / CLR_WIN are constants
;   - SPEC_LEFT_PAD is the left padding in characters
;   - Each bar occupies 3 columns, but only 2 are drawn
;======================================================================

_spectrum_render::
        push    ix
        push    iy

        ld      bc, #PORT_PAGE3
        in      a, (c)
        push    af
        push    bc

        ld      a, (_s_pb_text_pg)
        out     (c), a

        ld      ix, #sr_row_params
        ld      a, #4
        ld      (sr_row_ctr), a

sr_row_loop:
        ;--------------------------------------------------------------
        ; Row index = 4 - row_ctr  -> 0,1,2,3
        ;--------------------------------------------------------------
        ld      a, (sr_row_ctr)
        ld      c, a
        ld      a, #4
        sub     c                      ; A = row index
        add     a, a                   ; A = row index * 2
        ld      e, a                   
        ld      d, #0                  ; DE = row index * 2 as 16-bit offset
        push    de                     ; save for later use

        ;--------------------------------------------------------------
        ; char_ptr = 0xC000 + s_spec_char_ofs[row] + SPEC_LEFT_PAD
        ;--------------------------------------------------------------
        ld      hl, #_s_spec_char_ofs
        add     hl, de                 ; HL = &s_spec_char_ofs[row]
        ld      a, (hl)                
        inc     hl
        ld      h, (hl)
        ld      l, a                   ; HL = s_spec_char_ofs[row]

        ld      bc, #(0xC000 + SPEC_LEFT_PAD)
        add     hl, bc                 ; HL = char_ptr


        ;--------------------------------------------------------------
        ; attr_ptr = 0xC000 + s_spec_attr_ofs[row] + SPEC_LEFT_PAD
        ;--------------------------------------------------------------
        pop     de                      ; restore DE = row index * 2
        push    hl                      ; save char_ptr (HL) for later

        ld      hl, #_s_spec_attr_ofs
        add     hl, de
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        ld      bc, #(0xC000 + SPEC_LEFT_PAD)
        add     hl, bc
        push    hl
        pop     iy                     ; IY = attr_ptr        

        ;--------------------------------------------------------------
        ; Load row params: full threshold, half threshold, color
        ; Table layout: full, half, color
        ; IX - sr_row_params
        ; DE -char_ptr, IY - attr_ptr
        ;--------------------------------------------------------------
        ld      hl, #_spectrum_levels
        ld      b,  #SPEC_BARS

sr_bar_loop:
        ld      c, 0(ix)              ; thresh_full
        ld      d, 1(ix)              ; thresh_half
        ld      e, 2(ix)              ; row color

        ld      a, (hl)

        cp      c
        jr      nc, sr_full

        cp      d
        jr      nc, sr_half

sr_empty:
        pop     de                     ; DE = char_ptr
        ld      a, #' '
        ld      (de), a
        inc     de
        ld      (de), a

        ld      a, #CLR_WIN
        ld      0(iy), a
        ld      1(iy), a
        jr      sr_next

sr_half:
        ; attr
        ld      0(iy), e               ; row color
        ld      1(iy), e               ; row color
        pop     de                     ; DE = char_ptr
        ld      a, #CHAR_HALF
        ld      (de), a
        inc     de
        ld      (de), a
        jr      sr_next

sr_full:
        ld      0(iy), e               ; row color
        ld      1(iy), e               ; row color
        pop     de                     ; DE = char_ptr
        ld      a, #CHAR_FULL
        ld      (de), a
        inc     de
        ld      (de), a        

sr_next:
        inc     hl                     ; next spectrum level
        inc     de                     
        inc     de                     
        push    de                     ; next char position (2 columns per bar)
        inc     iy
        inc     iy
        inc     iy                     ; stride = 3

        djnz    sr_bar_loop

        inc     ix
        inc     ix
        inc     ix

        pop     de

        ld      a, (sr_row_ctr)
        dec     a
        ld      (sr_row_ctr), a
        jp      nz, sr_row_loop

        pop     bc
        pop     af
        out     (c), a

        pop     iy
        pop     ix
        ret

;======================================================================
; spectrum_opl_b0(reg, val) — A=reg, L=val
;
; OPL Bank0: B0-B8 key-on + 0xBD percussion (rising edge).
; Sets bits in spec_mask.
;======================================================================
_spectrum_opl_b0::
        ; Save reg and val
        ld      d, a                    ; D = reg
        ld      e, l                    ; E = val

        ; ── Check B0-B8 key-on ──
        cp      a, #0xB0
        jr      c, sob0_check_bd       ; reg < 0xB0
        cp      a, #0xB9
        jr      nc, sob0_check_bd      ; reg > 0xB8
        bit     5, e                    ; KeyOn bit in val
        jr      z, sob0_check_bd       ; not keyed on

        ; bar = (val >> 1) & 0x0F
        ld      a, e
        srl     a
        and     a, #0x0F
        ; spec_mask |= (1 << bar)
        call    _set_spec_bit
        ; Fall through to check BD

sob0_check_bd:
        ld      a, d
        cp      a, #0xBD
        ret     nz                      ; not reg 0xBD, done

        ; ── 0xBD percussion handling ──
        bit     5, e                    ; rhythm mode ON?
        jr      z, sob0_save_bd         ; no → just save shadow

        ; rising = val & ~old & 0x1F
        ld      a, (_spec_opl_bd)
        cpl                             ; ~old
        and     a, e                    ; val & ~old
        and     a, #0x1F               ; mask lower 5 bits
        jr      z, sob0_save_bd         ; no rising edges

        ; Check each percussion bit
        ld      b, a                    ; B = rising edges mask
        bit     4, b                    ; BD?
        jr      z, sob0_no_bd
        ld      a, #2                   ; BD → bar 2
        call    _set_spec_bit
sob0_no_bd:
        bit     3, b                    ; SD?
        jr      z, sob0_no_sd
        ld      a, #8                   ; SD → bar 8
        call    _set_spec_bit
sob0_no_sd:
        bit     2, b                    ; TOM?
        jr      z, sob0_no_tom
        ld      a, #5                   ; TOM → bar 5
        call    _set_spec_bit
sob0_no_tom:
        bit     1, b                    ; CY?
        jr      z, sob0_no_cy
        ld      a, #12                  ; CY → bar 12
        call    _set_spec_bit
sob0_no_cy:
        bit     0, b                    ; HH?
        jr      z, sob0_save_bd
        ld      a, #14                  ; HH → bar 14
        call    _set_spec_bit

sob0_save_bd:
        ld      a, e
        ld      (_spec_opl_bd), a
        ret

;======================================================================
; spectrum_opl_b1(reg, val) — A=reg, L=val
;
; OPL Bank1: B0-B6 key-on only. Same logic, smaller range.
;======================================================================
_spectrum_opl_b1::
        cp      a, #0xB0
        ret     c                       ; reg < 0xB0
        cp      a, #0xB7
        ret     nc                      ; reg > 0xB6
        bit     5, l                    ; KeyOn bit
        ret     z
        ; bar = (val >> 1) & 0x0F
        ld      a, l
        srl     a
        and     a, #0x0F
        jr      _set_spec_bit          ; tail-call

;======================================================================
; _set_spec_bit — set bit A (0..15) in spec_mask
; Input: A = bit number (0..15)
; Trashes: A, HL
;======================================================================
_set_spec_bit:
        ld      hl, (_spec_mask)
        cp      a, #8
        jr      nc, ssb_hi

        push    bc
        ld      b, a
        ld      a, b
        or      a
        jr      z, ssb_lo_done

        ld      a, #1
ssb_lo_shift:
        add     a, a
        djnz    ssb_lo_shift

ssb_lo_done:
        or      l
        ld      l, a
        pop     bc
        ld      (_spec_mask), hl
        ret

ssb_hi:
        sub     a, #8
        push    bc
        ld      b, a
        ld      a, b
        or      a
        jr      z, ssb_hi_done

        ld      a, #1
ssb_hi_shift:
        add     a, a
        djnz    ssb_hi_shift

ssb_hi_done:
        or      h
        ld      h, a
        pop     bc
        ld      (_spec_mask), hl
        ret

;======================================================================
; spectrum_ay(reg, val, chip2)  — A=reg, L=val, stack+2=chip2
;
; AY: shadow tone period (regs 0-5), trigger bar on volume (regs 8-10)
; YM2203 FM: shadow block (0xA4-0xA6), key-on (0x28)
;======================================================================
_spectrum_ay::
        ld      d, a                    ; D = reg
        ld      e, l                    ; E = val

        ; Compute base = chip2 ? 3 : 0
        ld      hl, #2
        add     hl, sp
        ld      a, (hl)                 ; A = chip2
        or      a, a
        ld      a, #0
        jr      z, say_base0
        ld      a, #3
say_base0:
        ld      (say_base), a           ; save base

        ; ── Check reg <= 5 (tone period shadow) ──
        ld      a, d                    ; A = reg
        cp      a, #6
        jr      nc, say_check_vol

        ; ch = base + (reg >> 1)
        srl     a                       ; A = reg >> 1
        ld      hl, #say_base
        add     a, (hl)                 ; A = base + (reg>>1) = ch index
        ; Offset into spec_ay_period[ch] (uint16_t array)
        add     a, a                    ; A = ch * 2
        ld      hl, #_spec_ay_period
        add     a, l
        ld      l, a
        jr      nc, say_nc1
        inc     h
say_nc1:
        ; HL → spec_ay_period[ch]
        bit     0, d                    ; reg & 1 ?
        jr      nz, say_hi_nibble

        ; Low byte: period = (period & 0x0F00) | val
        ld      a, e                    ; A = val
        ld      (hl), a                 ; write low byte
        ret

say_hi_nibble:
        ; High nibble: period = (period & 0x00FF) | ((val & 0x0F) << 8)
        inc     hl                      ; point to high byte
        ld      a, e
        and     a, #0x0F
        ld      (hl), a
        ret

        ; ── Check reg 8-10 (volume → trigger bar) ──
say_check_vol:
        ld      a, d
        cp      a, #8
        jr      c, say_check_fm_block
        cp      a, #11
        jr      nc, say_check_fm_block
        ; Check val & 0x0F != 0
        ld      a, e
        and     a, #0x0F
        ret     z                       ; volume=0, skip

        ; ch_index = base + (reg - 8)
        ld      a, d
        sub     a, #8
        ld      hl, #say_base
        add     a, (hl)                 ; A = array index
        ; Load spec_ay_period[index]
        add     a, a                    ; * 2
        ld      hl, #_spec_ay_period
        add     a, l
        ld      l, a
        jr      nc, say_nc2
        inc     h
say_nc2:
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a                    ; HL = period
        ; Fall through to spec_period_to_bar + set_spec_bit
        call    _spec_period_to_bar     ; A = bar index
        jp      _set_spec_bit           ; tail-call

        ; ── Check FM block shadow (regs 0xA4-0xA6) ──
say_check_fm_block:
        ld      a, d
        cp      a, #0xA4
        jr      c, say_check_keyon
        cp      a, #0xA7
        jr      nc, say_check_keyon
        ; spec_fm_block[base + (reg - 0xA4)] = (val >> 3) & 0x07
        sub     a, #0xA4
        ld      hl, #say_base
        add     a, (hl)                 ; A = index
        ld      hl, #_spec_fm_block
        add     a, l
        ld      l, a
        jr      nc, say_nc3
        inc     h
say_nc3:
        ld      a, e
        srl     a
        srl     a
        srl     a
        and     a, #0x07
        ld      (hl), a
        ret

        ; ── Check FM key-on (reg 0x28) ──
say_check_keyon:
        ld      a, d
        cp      a, #0x28
        ret     nz
        ; Check operators keyed on (val & 0xF0)
        ld      a, e
        and     a, #0xF0
        ret     z
        ; ch = val & 0x03
        ld      a, e
        and     a, #0x03
        cp      a, #3
        ret     nc                      ; ch > 2 invalid
        ; bar = spec_fm_block[base + ch] << 1
        ld      hl, #say_base
        add     a, (hl)                 ; A = index
        ld      hl, #_spec_fm_block
        add     a, l
        ld      l, a
        jr      nc, say_nc4
        inc     h
say_nc4:
        ld      a, (hl)                 ; A = block (0..7)
        add     a, a                    ; A = block * 2 = bar
        jp      _set_spec_bit           ; tail-call

;======================================================================
; spec_period_to_bar(period) — HL=period, returns A=bar (0..15)
;
; Cascaded compare on high byte first, then low byte.
; Optimised: single compare chain, no function calls.
;======================================================================
_spec_period_to_bar::
        ld      a, h                    ; A = period >> 8
        cp      a, #12
        jr      c, sptb_lt12
        xor     a, a                    ; return 0
        ret
sptb_lt12:
        cp      a, #8
        jr      c, sptb_lt8
        ld      a, #1
        ret
sptb_lt8:
        cp      a, #6
        jr      c, sptb_lt6
        ld      a, #2
        ret
sptb_lt6:
        cp      a, #4
        jr      c, sptb_lt4
        ld      a, #3
        ret
sptb_lt4:
        cp      a, #3
        jr      c, sptb_lt3
        ld      a, #4
        ret
sptb_lt3:
        cp      a, #2
        jr      c, sptb_lt2
        ld      a, #5
        ret
sptb_lt2:
        cp      a, #1
        jr      c, sptb_lt1
        ; h == 1: check low byte
        ld      a, l
        cp      a, #128
        ld      a, #6
        ret     nc                      ; period >= 384 (1*256+128)
        ld      a, #7
        ret                             ; period >= 256

sptb_lt1:
        ; h == 0: use low byte
        ld      a, l
        cp      a, #192
        jr      c, sptb_p1
        ld      a, #8
        ret
sptb_p1:
        cp      a, #128
        jr      c, sptb_p2
        ld      a, #9
        ret
sptb_p2:
        cp      a, #96
        jr      c, sptb_p3
        ld      a, #10
        ret
sptb_p3:
        cp      a, #64
        jr      c, sptb_p4
        ld      a, #11
        ret
sptb_p4:
        cp      a, #48
        jr      c, sptb_p5
        ld      a, #12
        ret
sptb_p5:
        cp      a, #32
        jr      c, sptb_p6
        ld      a, #13
        ret
sptb_p6:
        cp      a, #16
        jr      c, sptb_p7
        ld      a, #14
        ret
sptb_p7:
        ld      a, #15
        ret

;======================================================================
; spectrum_saa(reg, val) — A=reg, L=val
;
; SAA1099: shadow octave (regs 0x10-0x12), trigger on amplitude (0-5).
;======================================================================
_spectrum_saa::
        ld      d, a                    ; D = reg
        ld      e, l                    ; E = val

        ; ── Check octave shadow (regs 0x10-0x12) ──
        cp      a, #0x10
        jr      c, ssaa_check_amp
        cp      a, #0x13
        jr      nc, ssaa_check_amp

        ; ch = (reg - 0x10) * 2
        sub     a, #0x10
        add     a, a                    ; A = (reg-0x10)*2 = ch_base
        ld      hl, #_spec_saa_oct
        add     a, l
        ld      l, a
        jr      nc, ssaa_nc1
        inc     h
ssaa_nc1:
        ; spec_saa_oct[ch] = val & 0x07
        ld      a, e
        and     a, #0x07
        ld      (hl), a
        ; spec_saa_oct[ch+1] = (val >> 4) & 0x07
        inc     hl
        ld      a, e
        rrca
        rrca
        rrca
        rrca
        and     a, #0x07
        ld      (hl), a
        ret

        ; ── Check amplitude (regs 0x00-0x05) ──
ssaa_check_amp:
        ld      a, d
        cp      a, #0x06
        ret     nc                      ; reg >= 6, not amplitude
        ; Check val != 0
        ld      a, e
        or      a, a
        ret     z
        ; bar = spec_saa_oct[reg] << 1
        ld      a, d                    ; A = reg (0..5)
        ld      hl, #_spec_saa_oct
        add     a, l
        ld      l, a
        jr      nc, ssaa_nc2
        inc     h
ssaa_nc2:
        ld      a, (hl)                 ; A = octave
        add     a, a                    ; A = octave * 2 = bar
        jp      _set_spec_bit           ; tail-call

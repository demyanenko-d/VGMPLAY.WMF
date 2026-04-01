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
        .globl  _spectrum_decay
        .globl  _spectrum_decay_reset
        .globl  _spectrum_opl_b0
        .globl  _spectrum_opl_b1
        .globl  _spectrum_ay
        .globl  _spectrum_saa
        .globl  _spec_period_to_bar
        .globl  _spectrum_font_init
        .globl  _spectrum_font_restore

; ── Imports ──────────────────────────────────────────────────────────
        .globl  _spectrum_levels        ; uint8_t[16] in isr.s _DATA
        .globl  _isr_tick_ctr           ; volatile uint8_t in isr.s _DATA
        .globl  _s_pb_text_pg           ; uint8_t in main.c _DATA
        .globl  _s_spec_char_ofs        ; uint16_t[4] in main.c _DATA
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
CHAR_FULL    = 0x02      ; custom glyph: full-height striped bar
CHAR_HALF    = 0x01      ; custom glyph: half-height striped bar
CLR_WIN      = 0x70      ; WC_COLOR(WC_WHITE, WC_BLACK)
WC_PAGE_FONT0 = 0x01     ; font page (TSConf page number)
FONT_BASE     = 0xC000   ; page3 window base address
ISR_TICKS_PER_FRAME = 14  ; ticks per display frame (from variant_cfg.h)
DECAY_FAST  = 2           ; frames between decay for levels >= 6
DECAY_MED   = 5           ; frames between decay for levels 3-5
DECAY_SLOW  = 7          ; frames between decay for levels 1-2

;======================================================================
; Scratch variables in _DATA (kept minimal)
;======================================================================
        .area   _DATA

say_base:       .db 0                   ; temp: chip base index (0 or 3)
sr_row_ctr:     .db 0                   ; temp: row iteration counter
sr_rp_ptr:      .dw 0                   ; temp: current row-params pointer
sd_last_tick:   .db 0                   ; isr_tick_ctr snapshot for frame gate
sd_fast_ctr:    .db DECAY_FAST          ; countdown: decay levels >= 6
sd_med_ctr:     .db DECAY_MED           ; countdown: decay levels 3-5
sd_slow_ctr:    .db DECAY_SLOW          ; countdown: decay levels 1-2
sfont_save:     .ds 16                  ; backup of original font glyphs 0x01-0x02

        .area   _CODE

; Custom glyph bitmaps (8 bytes each) — stored in CODE, copied to font page.
glyph_half:                             ; char 0x01: half-height striped bar
        .db     0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00
glyph_full:                             ; char 0x02: full-height striped bar
        .db     0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00

; Row parameter table: [thresh_full, thresh_half, color] × 4 rows
; Row 0 (top=peak): thresholds 8,7  color=BRIGHT_RED
; Row 1: thresholds 6,5  color=BRIGHT_YELLOW
; Row 2: thresholds 4,3  color=BRIGHT_GREEN
; Row 3 (bottom): thresholds 2,1  color=GREEN
sr_row_params:
        .db     8, 7, 0x7A             ; row 0: white bg + bright red
        .db     6, 5, 0x7E             ; row 1: white bg + bright yellow
        .db     4, 3, 0x7C             ; row 2: white bg + bright green
        .db     2, 1, 0x74             ; row 3: white bg + green

;======================================================================
; spectrum_render(void)
;
; Optimised Z80 register layout — zero push/pop in inner bar loop:
;   IX = char_ptr  (stride +3 per bar via: inc ix x3)
;   IY = attr_ptr  (stride +3 per bar via: inc iy x3)
;   HL = spectrum_levels + bar_index
;   B  = bar counter (DJNZ)
;   C  = thresh_full  (row-constant, set ONCE before bar loop)
;   D  = thresh_half  (row-constant, set ONCE before bar loop)
;   E  = color        (row-constant, set ONCE before bar loop)
;
; Old inner loop loaded 0(ix)/1(ix)/2(ix) on every bar = 3x19=57 T
; per bar x 16 bars x 4 rows = 3648 wasted T-states per frame.
; New inner loop: C/D/E loaded once per row, never reloaded per bar.
; Total savings: ~3500 T-states per spectrum_render call.
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

        ld      a, #4
        ld      (sr_row_ctr), a
        ld      hl, #sr_row_params
        ld      (sr_rp_ptr), hl

sr_row_loop:
        ;--------------------------------------------------------------
        ; row_index = 4 - row_ctr  (0..3)
        ; Compute IX = char_ptr, IY = attr_ptr for this row.
        ;--------------------------------------------------------------
        ld      a, (sr_row_ctr)
        ld      e, a
        ld      a, #4
        sub     e
        add     a, a                    ; A = row_index * 2
        ld      e, a
        ld      d, #0

        ; IX = 0xC000 + s_spec_char_ofs[row_index] + SPEC_LEFT_PAD
        ld      hl, #_s_spec_char_ofs
        add     hl, de
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a                    ; HL = s_spec_char_ofs[row_index]
        ld      bc, #(0xC000 + SPEC_LEFT_PAD)
        add     hl, bc
        push    hl
        pop     ix                      ; IX = char_ptr

        ; IY = IX + 0x0080 (attr_ofs = char_ofs | 0x0080 in TSConf)
        push    ix
        pop     hl
        ld      de, #0x0080
        add     hl, de
        push    hl
        pop     iy                      ; IY = attr_ptr

        ;--------------------------------------------------------------
        ; Load row params ONCE into C/D/E: all three are row-constants.
        ; C, D, E survive the bar loop untouched (djnz only changes B).
        ;--------------------------------------------------------------
        ld      hl, (sr_rp_ptr)
        ld      c, (hl)                 ; C = thresh_full
        inc     hl
        ld      d, (hl)                 ; D = thresh_half
        inc     hl
        ld      e, (hl)                 ; E = color
        inc     hl
        ld      (sr_rp_ptr), hl         ; advance for next row

        ;--------------------------------------------------------------
        ; Bar loop: 16 bars — all comparisons use stable C,D,E regs.
        ; No push/pop needed: all state lives in IX, IY, HL, B, C, D, E.
        ;--------------------------------------------------------------
        ld      hl, #_spectrum_levels
        ld      b,  #SPEC_BARS

sr_bar_loop:
        ld      a, (hl)
        cp      c                       ; NC if level >= thresh_full
        jr      nc, sr_full
        cp      d                       ; NC if level >= thresh_half
        jr      nc, sr_half

sr_empty:
        ld      a, #' '
        ld      0(ix), a
        ld      1(ix), a
        ld      a, #CLR_WIN
        ld      0(iy), a
        ld      1(iy), a
        jr      sr_next

sr_half:
        ld      a, #CHAR_HALF
        ld      0(ix), a
        ld      1(ix), a
        ld      0(iy), e                ; E = row color (stable per row)
        ld      1(iy), e
        jr      sr_next

sr_full:
        ld      a, #CHAR_FULL
        ld      0(ix), a
        ld      1(ix), a
        ld      0(iy), e                ; E = row color (stable per row)
        ld      1(iy), e

sr_next:
        inc     hl                      ; next spectrum level
        inc     ix                      ; char_ptr += SPEC_BAR_W (3)
        inc     ix
        inc     ix
        inc     iy                      ; attr_ptr += SPEC_BAR_W (3)
        inc     iy
        inc     iy
        djnz    sr_bar_loop

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
; spectrum_decay_reset() — initialize decay timers at song start
;======================================================================
_spectrum_decay_reset::
        ld      a, (_isr_tick_ctr)
        ld      (sd_last_tick), a
        ld      a, #DECAY_FAST
        ld      (sd_fast_ctr), a
        ld      a, #DECAY_MED
        ld      (sd_med_ctr), a
        ld      a, #DECAY_SLOW
        ld      (sd_slow_ctr), a
        ret

;======================================================================
; spectrum_decay() — frame-gated non-linear level decay
;
; Called every main-loop iteration; returns immediately if less than
; ISR_TICKS_PER_FRAME ticks have elapsed since the last frame.
;
; Three countdown timers control decay speed per level range:
;   Levels >= 6 : -1 every  3 frames  (~60 ms)
;   Levels  3-5 : -1 every  6 frames  (~180 ms)
;   Levels  1-2 : -1 every 10 frames  (~200 ms)
;
; Register layout:
;   C = decay-flags bitmask (bit2=fast, bit1=med, bit0=slow)
;   B = bar loop counter
;   HL = pointer into spectrum_levels[]
;======================================================================
_spectrum_decay::
        ; ── Frame gate ──────────────────────────────────────────────
        ld      a, (_isr_tick_ctr)
        ld      hl, #sd_last_tick
        sub     (hl)                    ; elapsed = now - last
        cp      #ISR_TICKS_PER_FRAME
        ret     c                       ; < 1 frame, skip

        ; Advance by one frame
        ld      a, (hl)
        add     a, #ISR_TICKS_PER_FRAME
        ld      (hl), a

        ; ── Build decay-flags in C ──────────────────────────────────
        ld      c, #0

        ld      hl, #sd_fast_ctr        ; 5-frame timer (levels >= 6)
        dec     (hl)
        jr      nz, sd_nf
        ld      (hl), #DECAY_FAST
        set     2, c
sd_nf:
        ld      hl, #sd_med_ctr         ; med-frame timer (levels 3-5)
        dec     (hl)
        jr      nz, sd_nm
        ld      (hl), #DECAY_MED
        set     1, c
sd_nm:
        ld      hl, #sd_slow_ctr        ; slow-frame timer (levels 1-2)
        dec     (hl)
        jr      nz, sd_ns
        ld      (hl), #DECAY_SLOW
        set     0, c
sd_ns:
        ld      a, c
        or      a
        ret     z                       ; no tier expired → nothing to decay

        ; ── Single loop over 16 bars ────────────────────────────────
        ld      hl, #_spectrum_levels
        ld      b, #SPEC_BARS

sd_bar_loop:
        ld      a, (hl)
        or      a
        jr      z, sd_next              ; level 0 → skip

        cp      #6
        jr      nc, sd_high             ; level >= 6 → fast tier

        cp      #3
        jr      nc, sd_mid              ; level >= 3 → medium tier

        ; level 1-2: slow tier
        bit     0, c
        jr      z, sd_next
        dec     (hl)
        jr      sd_next

sd_mid:
        bit     1, c
        jr      z, sd_next
        dec     (hl)
        jr      sd_next

sd_high:
        bit     2, c
        jr      z, sd_next
        dec     (hl)

sd_next:
        inc     hl
        djnz    sd_bar_loop
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

; 16-bit mask table — direct 16-bit OR into spec_mask, no byte split.
; 32 bytes ROM, but eliminates push/pop bc + bit3 branch in hot path.
ssb_mask_table:
        .dw     0x0001, 0x0002, 0x0004, 0x0008
        .dw     0x0010, 0x0020, 0x0040, 0x0080
        .dw     0x0100, 0x0200, 0x0400, 0x0800
        .dw     0x1000, 0x2000, 0x4000, 0x8000

;======================================================================
; _set_spec_bit — set bit A (0..15) in spec_mask
; Input: A = bit number (0..15)
; Trashes: A, D, E, HL
; Preserves: B, C (no push/pop needed)
; Cost: ~122 T-states flat.
;======================================================================
_set_spec_bit:
        add     a, a                    ; A = bit * 2 (index into word table)
        ld      e, a
        ld      d, #0
        ld      hl, #ssb_mask_table
        add     hl, de                  ; HL → mask entry
        ld      e, (hl)                 ; E = mask lo
        inc     hl
        ld      d, (hl)                 ; D = mask hi
        ld      hl, (_spec_mask)
        ld      a, l
        or      e
        ld      l, a
        ld      a, h
        or      d
        ld      h, a
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

;======================================================================
; spectrum_font_init() — install custom glyphs for chars 0x01-0x02
;
; Maps font page into page3 window, saves original 16 bytes (2 glyphs),
; writes striped-bar patterns from glyph_half / glyph_full.
; Must be called once before playback. Pairs with spectrum_font_restore.
;======================================================================
_spectrum_font_init::
        ld      bc, #PORT_PAGE3
        in      a, (c)
        push    af
        push    bc

        ; Map font page
        ld      a, #WC_PAGE_FONT0
        out     (c), a

        ; Save original glyphs 0x01-0x02 (16 bytes at FONT_BASE+8)
        ld      hl, #(FONT_BASE + 8)   ; glyph 0x01 starts at char*8
        ld      de, #sfont_save
        ld      bc, #16
        ldir

        ; Write custom glyph 0x01 (half-height striped bar)
        ld      hl, #glyph_half
        ld      de, #(FONT_BASE + 8)
        ld      bc, #8
        ldir

        ; Write custom glyph 0x02 (full-height striped bar)
        ld      hl, #glyph_full
        ld      de, #(FONT_BASE + 16)
        ld      bc, #8
        ldir

        pop     bc
        pop     af
        out     (c), a                  ; restore page3
        ret

;======================================================================
; spectrum_font_restore() — restore original glyphs for chars 0x01-0x02
;
; Must be called on exit to avoid corrupting the system font.
;======================================================================
_spectrum_font_restore::
        ld      bc, #PORT_PAGE3
        in      a, (c)
        push    af
        push    bc

        ld      a, #WC_PAGE_FONT0
        out     (c), a

        ; Restore original 16 bytes
        ld      hl, #sfont_save
        ld      de, #(FONT_BASE + 8)
        ld      bc, #16
        ldir

        pop     bc
        pop     af
        out     (c), a                  ; restore page3
        ret

;==============================================================================
; @file   opl3.s
; @brief  OPL3/AY/SAA register operations for SDCC Z80 (sdcccall(1))
;
; Порты TSConfig для OPL3:
;   #C4 = Bank 0 Address   #C5 = Bank 0 Data
;   #C6 = Bank 1 Address   #C7 = Bank 1 Data
;==============================================================================

        .module opl3
        .area _CODE

;----------------------------------------------------------------------
; opl3_write_b0(uint8_t addr, uint8_t data) — Bank 0
;----------------------------------------------------------------------
        .globl _opl3_write_b0
_opl3_write_b0::
        ld      c, #0xC4
        jr      _opl3_write

;----------------------------------------------------------------------
; opl3_write_b1(uint8_t addr, uint8_t data) — Bank 1
;----------------------------------------------------------------------
        .globl _opl3_write_b1
_opl3_write_b1::
        ld      c, #0xC6
_opl3_write:
        out     (c), a              ; addr (B don't-care for C4-C7)
        ld      hl, #2
        add     hl, sp
        ld      a, (hl)
        inc     c
        out     (c), a              ; data
        ret

;----------------------------------------------------------------------
; tsconf_out(uint16_t port, uint8_t val)
;----------------------------------------------------------------------
        .globl _tsconf_out
_tsconf_out::
        ld      b, d
        ld      c, e
        ld      hl, #2
        add     hl, sp
        ld      a, (hl)
        out     (c), a
        ret

;----------------------------------------------------------------------
; opl3_init() — OPL3 enable + wavesel + reset (falls through)
;----------------------------------------------------------------------
        .globl _opl3_init
_opl3_init::
        ld      a, #0x05
        ld      c, #0xC6
        out     (c), a              ; B don't-care for C4-C7
        ld      a, #0x01            ; NEW bit only (OPL3 enable)
        inc     c
        out     (c), a
        ld      a, #0x01
        ld      c, #0xC4
        out     (c), a
        ld      a, #0x20
        inc     c
        out     (c), a
        ; fall through to opl3_reset

;----------------------------------------------------------------------
; opl3_reset() — Full OPL3 silence: zero ALL significant registers
;   Both banks: Key-Off (B0-B8=0), Total Level max atten (40-55=3F),
;   Percussion off (BD=0), F-Num (A0-A8=0), Feedback (C0-C8=0),
;   AM/Vib/Mult (20-35=0), AR/DR (60-75=0), SL/RR (80-95=0),
;   Waveform (E0-F5=0).
;   Uses _opl3_zero_range and _opl3_fill_range helpers.
;----------------------------------------------------------------------
        .globl _opl3_reset
_opl3_reset::
        ; ── Bank 0 ──
        ld      c, #0xC4
        call    _opl3_reset_bank

        ; Percussion off: BD = 0 (Bank 0 only)
        ld      a, #0xBD
        out     (c), a
        inc     c
        xor     a, a
        out     (c), a
        dec     c

        ; ── Bank 1 ──
        ld      c, #0xC6
        ; fall through to _opl3_reset_bank

;----------------------------------------------------------------------
; _opl3_reset_bank — zero all significant regs of one OPL3 bank
;   C = addr port (0xC4 or 0xC6)
;   Destroys: A, B, D, E
;----------------------------------------------------------------------
_opl3_reset_bank:
        ; KeyOff: B0-B8 = 0
        ld      d, #0xB0
        ld      e, #9
        call    _opl3_zero_range

        ; Total Level max attenuation: 40-55 = 0x3F
        ld      d, #0x40
        ld      e, #22
        ld      a, #0x3F
        call    _opl3_fill_range

        ; F-Num lo: A0-A8 = 0
        ld      d, #0xA0
        ld      e, #9
        call    _opl3_zero_range

        ; Feedback/Output: C0-C8 = 0
        ld      d, #0xC0
        ld      e, #9
        call    _opl3_zero_range

        ; AM/Vib/Mult: 20-35 = 0
        ld      d, #0x20
        ld      e, #22
        call    _opl3_zero_range

        ; AR/DR: 60-75 = 0
        ld      d, #0x60
        ld      e, #22
        call    _opl3_zero_range

        ; SL/RR: 80-95 = 0
        ld      d, #0x80
        ld      e, #22
        call    _opl3_zero_range

        ; Waveform: E0-F5 = 0
        ld      d, #0xE0
        ld      e, #22
        jr      _opl3_zero_range    ; tail-call → ret

;----------------------------------------------------------------------
; _opl3_zero_range — fill E consecutive regs starting at D with 0
;   C = port, D = start reg, E = count
;----------------------------------------------------------------------
_opl3_zero_range:
        xor     a, a
        ; fall through
;----------------------------------------------------------------------
; _opl3_fill_range — fill E consecutive regs starting at D with value A
;   C = port (0xC4 for Bank0, 0xC6 for Bank1)
;   D = start register, E = count, A = value to write
;   Destroys: A, B, D, E
;----------------------------------------------------------------------
_opl3_fill_range:
        ld      b, a                ; B = value to write
_opl3_fr_lp:
        ld      a, d
        out     (c), a              ; write register address
        inc     c
        ld      a, b
        out     (c), a              ; write value
        dec     c
        inc     d
        dec     e
        jr      nz, _opl3_fr_lp
        ret

;----------------------------------------------------------------------
; Общая подпрограмма: silence текущего AY чипа.  C уже = 0xFD.
;   Reg 7 = 0x3F (mixer off), Regs 8-10 = 0 (volume off)
;----------------------------------------------------------------------
_ay_silence_core:
        ld      b, #0xFF
        ld      a, #7
        out     (c), a
        ld      b, #0xBF
        ld      a, #0x3F
        out     (c), a
        ld      d, #8
_ayc_lp:
        ld      b, #0xFF
        ld      a, d
        out     (c), a
        ld      b, #0xBF
        xor     a, a
        out     (c), a
        inc     d
        ld      a, d
        cp      a, #11
        jr      c, _ayc_lp
        ret

;----------------------------------------------------------------------
; ay_write_reg(uint8_t reg, uint8_t val) — write AY chip 1 register
;   reg in A, val on stack
;----------------------------------------------------------------------
        .globl _ay_write_reg
_ay_write_reg::
        ld      c, #0xFD
        ld      b, #0xFF
        out     (c), a              ; select register
        ld      hl, #2
        add     hl, sp
        ld      a, (hl)
        ld      b, #0xBF
        out     (c), a              ; write value
        ret

;----------------------------------------------------------------------
; ay2_write_reg(uint8_t reg, uint8_t val) — write AY chip 2 register
;----------------------------------------------------------------------
        .globl _ay2_write_reg
_ay2_write_reg::
        ld      c, #0xFD
        ld      b, #0xFF
        push    af                  ; save reg
        ld      a, #0xFE
        out     (c), a              ; select chip 2
        pop     af
        ld      b, #0xFF
        out     (c), a              ; select register
        ld      hl, #2
        add     hl, sp
        ld      a, (hl)
        ld      b, #0xBF
        out     (c), a              ; write value
        ld      b, #0xFF
        ld      a, #0xFF
        out     (c), a              ; switch back to chip 1
        ret

;----------------------------------------------------------------------
; saa_write_reg(uint8_t reg, uint8_t val) — SAA1099 chip 1
;   addr=#00FF, data=#01FF
;----------------------------------------------------------------------
        .globl _saa_write_reg
_saa_write_reg::
        ld      c, #0xFF
        ld      b, #0x00
        out     (c), a              ; addr = reg
        ld      hl, #2
        add     hl, sp
        ld      a, (hl)
        inc     b                   ; B = 0x01
        out     (c), a              ; data = val
        ret

;----------------------------------------------------------------------
; saa2_write_reg(uint8_t reg, uint8_t val) — SAA1099 chip 2
;   addr=#02FF, data=#03FF
;----------------------------------------------------------------------
        .globl _saa2_write_reg
_saa2_write_reg::
        ld      c, #0xFF
        ld      b, #0x02
        out     (c), a              ; addr = reg
        ld      hl, #2
        add     hl, sp
        ld      a, (hl)
        inc     b                   ; B = 0x03
        out     (c), a              ; data = val
        ret

;----------------------------------------------------------------------
; ay_silence() — AY chip 1  (FFFD/BFFD)
;----------------------------------------------------------------------
        .globl _ay_silence
_ay_silence::
        ld      c, #0xFD
        jr      _ay_silence_core

;----------------------------------------------------------------------
; ay2_silence() — TurboSound chip 2: select → silence → switch back
;----------------------------------------------------------------------
        .globl _ay2_silence
_ay2_silence::
        ld      c, #0xFD
        ld      b, #0xFF
        ld      a, #0xFE
        out     (c), a              ; select chip 2
        call    _ay_silence_core
        ld      b, #0xFF
        ld      a, #0xFF
        out     (c), a              ; switch back to chip 1
        ret

;----------------------------------------------------------------------
; Общая подпрограмма: silence SAA чипа.  C=0xFF, B=addr port hi.
;   Data port = B+1 (inc b / dec b).
;   Reset (0x1C=02), disable (0x1C=00), amplitude regs 0x00-0x05=0.
;----------------------------------------------------------------------
_saa_silence_core:
        ld      a, #0x1C
        out     (c), a              ; addr
        inc     b
        ld      a, #0x02
        out     (c), a              ; data: reset
        dec     b
        ld      a, #0x1C
        out     (c), a              ; addr
        inc     b
        xor     a, a
        out     (c), a              ; data: sound off
        dec     b
        ld      d, #0
_saac_lp:
        ld      a, d
        out     (c), a              ; addr: reg N
        inc     b
        xor     a, a
        out     (c), a              ; data: 0
        dec     b
        inc     d
        ld      a, d
        cp      a, #6
        jr      c, _saac_lp
        ret

;----------------------------------------------------------------------
; saa_silence() — SAA1099 chip 1  (addr=#00FF, data=#01FF)
;----------------------------------------------------------------------
        .globl _saa_silence
_saa_silence::
        ld      c, #0xFF
        ld      b, #0x00
        jr      _saa_silence_core

;----------------------------------------------------------------------
; saa2_silence() — SAA1099 chip 2  (addr=#02FF, data=#03FF)
;----------------------------------------------------------------------
        .globl _saa2_silence
_saa2_silence::
        ld      c, #0xFF
        ld      b, #0x02
        jr      _saa_silence_core

; ═══════════════════════════════════════════════════════════════════════════
; Keyboard Module - Direct ZX Spectrum matrix reading
; ═══════════════════════════════════════════════════════════════════════════
; Reads keyboard via IN port #FE (no WC API, no interrupts needed).
; TSConfig translates PS/2 → ZX matrix in hardware.
;
; ZX Spectrum keyboard matrix (active low):
;   Port #FEFE: CAPS, Z, X, C, V        (row 0)
;   Port #FDFE: A, S, D, F, G           (row 1)
;   Port #FBFE: Q, W, E, R, T           (row 2)
;   Port #F7FE: 1, 2, 3, 4, 5           (row 3)
;   Port #EFFE: 0, 9, 8, 7, 6           (row 4)
;   Port #DFFE: P, O, I, U, Y           (row 5)
;   Port #BFFE: ENTER, L, K, J, H       (row 6)
;   Port #7FFE: SPACE, SYM, M, N, B     (row 7)
;
; Each row: bit 0 = rightmost key, bit 4 = leftmost key
; Bit = 0 means key pressed, bit = 1 means not pressed
; ═══════════════════════════════════════════════════════════════════════════

; Debounce counter (simple frame-based debouncing)
KBD_DEBOUNCE:   DB 0

; ═══════════════════════════════════════════════════════════════════════════
; KBD_READ - Read keyboard and return action code
; ═══════════════════════════════════════════════════════════════════════════
; Output: A = action code
;   0 = nothing pressed
;   1 = ESC (Caps Shift + Space)
;   2 = next track (Space alone)
;   3 = prev track (P key)
;   4 = pause/unpause (M key)
; ═══════════════════════════════════════════════════════════════════════════
KBD_READ:
        ; Simple debouncing: skip reads for N frames after a keypress
        LD A,(KBD_DEBOUNCE)
        OR A
        JR Z,KBD_DO_READ
        DEC A
        LD (KBD_DEBOUNCE),A
        XOR A                   ; return 0
        RET

KBD_DO_READ:
        ; --- Check ESC = Caps Shift + Space ---
        ; Caps Shift = row 0 bit 0 (#FEFE)
        LD A,#FE
        IN A,(#FE)
        BIT 0,A                 ; Caps Shift
        JR NZ,KBD_CHECK_SPACE   ; not pressed

        ; Caps Shift is held. Check Space = row 7 bit 0 (#7FFE)
        LD A,#7F
        IN A,(#FE)
        BIT 0,A                 ; Space
        JR NZ,KBD_CHECK_SPACE   ; not pressed
        ; ESC detected!
        LD A,16
        LD (KBD_DEBOUNCE),A
        LD A,1
        RET

KBD_CHECK_SPACE:
        ; --- Check SPACE alone (without Caps Shift) ---
        ; Space = row 7 bit 0 (#7FFE)
        LD A,#7F
        IN A,(#FE)
        BIT 0,A
        JR NZ,KBD_CHECK_P

        ; Check that Caps Shift is NOT held (to distinguish from ESC)
        LD A,#FE
        IN A,(#FE)
        BIT 0,A                 ; CShift=0 means held
        JR Z,KBD_CHECK_P       ; CShift held → this is ESC, not SPACE

        ; SPACE alone = next track
        LD A,12
        LD (KBD_DEBOUNCE),A
        LD A,2
        RET

KBD_CHECK_P:
        ; --- Check P key = row 5 bit 0 (#DFFE) ---
        LD A,#DF
        IN A,(#FE)
        BIT 0,A
        JR NZ,KBD_CHECK_M
        ; P pressed = previous track
        LD A,12
        LD (KBD_DEBOUNCE),A
        LD A,3
        RET

KBD_CHECK_M:
        ; --- Check M key = row 7 bit 2 (#7FFE) ---
        LD A,#7F
        IN A,(#FE)
        BIT 2,A
        JR NZ,KBD_NOTHING
        ; M pressed = pause
        LD A,12
        LD (KBD_DEBOUNCE),A
        LD A,4
        RET

KBD_NOTHING:
        XOR A
        RET

; ═══════════════════════════════════════════════════════════════════════════
; OPL3 (YMF262) Hardware Driver - Simplified
; ═══════════════════════════════════════════════════════════════════════════
;
; OPL3_INIT  - Detect and initialize OPL3
; OPL3_RESET - Silence all channels
;
; Ports: #C4/#C5 = bank 0 addr/data, #C6/#C7 = bank 1 addr/data
; ═══════════════════════════════════════════════════════════════════════════

; ═══════════════════════════════════════════════════════════════════════════
; OPL3_INIT - Detect OPL3 and enable OPL3 mode
; ═══════════════════════════════════════════════════════════════════════════
; Output: OPL3_FOUND = 1 if found, 0 if not
OPL3_INIT:
        PUSH AF
        PUSH BC
        PUSH DE

        ; --- Detect OPL3 using timer method ---
        ; 1. Reset timers
        LD BC,OPL3_ADDR0
        LD A,#04                ; Timer control register
        OUT (C),A
        CALL OPL3_DELAY_SHORT

        LD BC,OPL3_DATA0
        LD A,#60                ; Reset T1 and T2 flags
        OUT (C),A
        CALL OPL3_DELAY_LONG

        ; 2. Read status - should be 0
        LD BC,OPL3_ADDR0
        IN A,(C)
        AND #E0
        JR NZ,OPL3_NOT_FOUND

        ; 3. Start timer 1 with max value
        LD BC,OPL3_ADDR0
        LD A,#02                ; Timer 1 register
        OUT (C),A
        CALL OPL3_DELAY_SHORT

        LD BC,OPL3_DATA0
        LD A,#FF
        OUT (C),A
        CALL OPL3_DELAY_LONG

        LD BC,OPL3_ADDR0
        LD A,#04
        OUT (C),A
        CALL OPL3_DELAY_SHORT

        LD BC,OPL3_DATA0
        LD A,#21                ; Start timer 1, unmask
        OUT (C),A

        ; 4. Wait for timer to overflow (~100 µs)
        LD B,200                ; generous delay at 14 MHz
OPL3_DET_WAIT:
        DJNZ OPL3_DET_WAIT

        ; 5. Read status
        LD BC,OPL3_ADDR0
        IN A,(C)
        AND #E0
        JR Z,OPL3_NOT_FOUND    ; timer didnt fire = no OPL

        ; 6. Reset timers again
        LD BC,OPL3_ADDR0
        LD A,#04
        OUT (C),A
        CALL OPL3_DELAY_SHORT

        LD BC,OPL3_DATA0
        LD A,#60
        OUT (C),A
        CALL OPL3_DELAY_LONG

        ; --- OPL3 Found! ---
        LD A,1
        LD (OPL3_FOUND),A

        ; Enable OPL3 mode (register 0x105 = bit 0)
        LD BC,OPL3_ADDR1
        LD A,#05
        OUT (C),A
        CALL OPL3_DELAY_SHORT

        LD BC,OPL3_DATA1
        LD A,#01
        OUT (C),A
        CALL OPL3_DELAY_LONG

        ; Waveform select enable (register 0x01 = 0x20)
        LD BC,OPL3_ADDR0
        LD A,#01
        OUT (C),A
        CALL OPL3_DELAY_SHORT

        LD BC,OPL3_DATA0
        LD A,#20
        OUT (C),A
        CALL OPL3_DELAY_LONG

        POP DE
        POP BC
        POP AF
        RET

OPL3_NOT_FOUND:
        XOR A
        LD (OPL3_FOUND),A
        POP DE
        POP BC
        POP AF
        RET

; ═══════════════════════════════════════════════════════════════════════════
; OPL3_RESET - Silence all OPL3 channels
; ═══════════════════════════════════════════════════════════════════════════
OPL3_RESET:
        PUSH AF
        PUSH BC
        PUSH DE

        LD A,(OPL3_FOUND)
        OR A
        JR Z,OPL3_RESET_DONE

        ; Write 0 to all registers in both banks
        ; Bank 0: registers #20-#F5
        LD D,#20                ; start register
OPL3_RESET_BANK0:
        LD BC,OPL3_ADDR0
        LD A,D
        OUT (C),A
        CALL OPL3_DELAY_SHORT

        LD BC,OPL3_DATA0
        XOR A
        OUT (C),A
        CALL OPL3_DELAY_LONG

        INC D
        LD A,D
        CP #F6
        JR C,OPL3_RESET_BANK0

        ; Bank 1: registers #20-#F5
        LD D,#20
OPL3_RESET_BANK1:
        LD BC,OPL3_ADDR1
        LD A,D
        OUT (C),A
        CALL OPL3_DELAY_SHORT

        LD BC,OPL3_DATA1
        XOR A
        OUT (C),A
        CALL OPL3_DELAY_LONG

        INC D
        LD A,D
        CP #F6
        JR C,OPL3_RESET_BANK1

        ; Disable OPL3 mode on exit
        LD BC,OPL3_ADDR1
        LD A,#05
        OUT (C),A
        CALL OPL3_DELAY_SHORT
        LD BC,OPL3_DATA1
        XOR A
        OUT (C),A
        CALL OPL3_DELAY_LONG

OPL3_RESET_DONE:
        POP DE
        POP BC
        POP AF
        RET

; ═══════════════════════════════════════════════════════════════════════════
; OPL3 Delays
; ═══════════════════════════════════════════════════════════════════════════
; At 14 MHz: ~3.3 µs for address = 46 cycles, ~22 µs for data = 308 cycles
; DJNZ = 13 cycles per iteration at 14 MHz (or 8+5)

OPL3_DELAY_SHORT:
        ; ~3.5 µs at 14 MHz = ~49 cycles
        PUSH BC
        LD B,5
OPL3_DS_LOOP:
        DJNZ OPL3_DS_LOOP
        POP BC
        RET

OPL3_DELAY_LONG:
        ; ~22 µs at 14 MHz = ~308 cycles
        PUSH BC
        LD B,25
OPL3_DL_LOOP:
        DJNZ OPL3_DL_LOOP
        POP BC
        RET

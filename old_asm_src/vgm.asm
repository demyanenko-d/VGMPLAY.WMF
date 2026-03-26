; ═══════════════════════════════════════════════════════════════════════════
; VGM Parser & Buffer Filler
; ═══════════════════════════════════════════════════════════════════════════
;
; - VGM_PARSE_HEADER: Parse VGM file header from VGMBUF
; - VGM_FILL_BUFFER:  Fill a command buffer (A=0 for BUF_A, A=1 for BUF_B)
;                     Reads VGM stream, converts to ISR commands
;
; VGM opcodes processed:
;   0x5E rr dd   - YMF262 port 0 write (addr=rr, data=dd)
;   0x5F rr dd   - YMF262 port 1 write (addr=rr, data=dd)
;   0x61 nn nn   - Wait N samples (16-bit LE)
;   0x62         - Wait 735 samples (1/60 sec)
;   0x63         - Wait 882 samples (1/50 sec)
;   0x66         - End of sound data
;   0x70-0x7F    - Wait 1-16 samples
;
; Command buffer format (4 bytes each, aligned):
;   CMD_WAIT  (0):   [0x00, ticks_lo, ticks_hi, 0x00]
;   CMD_WRITE (1):   [0x01, port,     value,    0x00]
;   CMD_END   (FF):  [0xFF, 0x00,     0x00,     0x00]
;
; Wait counter mapping: VGM "wait N samples" → ISR wait = N-1
; (because the ISR tick that processes the WAIT command is tick 0,
;  and counter=0 means "process next tick")
; ═══════════════════════════════════════════════════════════════════════════

; ═══════════════════════════════════════════════════════════════════════════
; VGM_PARSE_HEADER
; ═══════════════════════════════════════════════════════════════════════════
; Parse VGM header at VGMBUF.
; Output: Z = success, NZ = error
VGM_PARSE_HEADER:
        PUSH HL
        PUSH DE
        PUSH BC

        ; Check "Vgm " signature
        LD HL,VGMBUF
        LD A,(HL)
        CP 'V'
        JP NZ,VGM_PARSE_FAIL
        INC HL
        LD A,(HL)
        CP 'g'
        JP NZ,VGM_PARSE_FAIL
        INC HL
        LD A,(HL)
        CP 'm'
        JP NZ,VGM_PARSE_FAIL
        INC HL
        LD A,(HL)
        CP ' '
        JP NZ,VGM_PARSE_FAIL

        ; Read total samples (offset 0x18, 32-bit)
        LD HL,VGMBUF+#18
        LD E,(HL)
        INC HL
        LD D,(HL)
        LD (VGM_TOTAL_SAMPLES),DE
        INC HL
        LD E,(HL)
        INC HL
        LD D,(HL)
        LD (VGM_TOTAL_SAMPLES+2),DE

        ; Read OPL3 clock (offset 0x5C, 32-bit) - must be non-zero
        LD HL,VGMBUF+#5C
        LD E,(HL)
        INC HL
        LD D,(HL)
        LD (VGM_CLOCK),DE
        INC HL
        LD E,(HL)
        INC HL
        LD D,(HL)
        LD (VGM_CLOCK+2),DE

        ; Check OPL3 clock != 0 (at least low word should be non-zero for OPL3)
        LD HL,(VGM_CLOCK)
        LD A,H
        OR L
        JR NZ,VGM_HAS_OPL3
        LD HL,(VGM_CLOCK+2)
        LD A,H
        OR L
        ; OPL3 clock = 0: no OPL3 data, but well allow it (might still have data)
VGM_HAS_OPL3:

        ; Read data offset (offset 0x34, relative to 0x34)
        ; For VGM version < 1.50, data starts at 0x40
        LD HL,VGMBUF+#08       ; version
        LD A,(HL)               ; version low byte (BCD minor)
        LD B,A
        INC HL
        LD A,(HL)               ; version byte 2
        ; Check if version >= 1.50
        ; Version bytes: [08]=minor_lo, [09]=minor_hi, [0A]=major_lo, [0B]=major_hi
        ; For v1.50: bytes are 50 01 00 00 or similar
        ; Simplified: if offset 0x34 has non-zero value and version >= 1.50, use it
        LD HL,VGMBUF+#34
        LD E,(HL)
        INC HL
        LD D,(HL)
        ; data_offset = value at 0x34 (relative to 0x34)
        ; If 0, default to 0x0C (data starts at 0x40 = 0x34+0x0C)
        LD A,D
        OR E
        JR NZ,VGM_USE_DATA_OFF
        LD DE,#0C               ; default: 0x40 - 0x34 = 0x0C
VGM_USE_DATA_OFF:
        ; VGM_DATA_START = VGMBUF + 0x34 + data_offset
        LD HL,VGMBUF+#34
        ADD HL,DE
        LD (VGM_DATA_START),HL
        LD (VGM_READ_PTR),HL

        ; Read loop offset (offset 0x1C, relative to 0x1C)
        LD HL,VGMBUF+#1C
        LD E,(HL)
        INC HL
        LD D,(HL)
        LD A,D
        OR E
        JR Z,VGM_NO_LOOP
        ; VGM_LOOP_ADDR = #C000 + (abs_offset & #3FFF), VGM_LOOP_PAGE = abs_offset >> 14
        ; abs_offset = #1C + value_at_1C (16-битное; достаточно для loop-поинтов < 64KB от начала)
        LD HL,#001C
        ADD HL,DE               ; HL = абсолютное смещение точки петли
        LD A,H
        SRL A:SRL A:SRL A:SRL A:SRL A:SRL A  ; A = H >> 6 = номер VPL-страницы
        LD (VGM_LOOP_PAGE),A
        LD A,H
        AND #3F
        OR #C0
        LD H,A                  ; HL = #C000 + (offset & #3FFF)
        LD (VGM_LOOP_ADDR),HL
        JR VGM_LOOP_DONE
VGM_NO_LOOP:
        LD HL,0
        LD (VGM_LOOP_ADDR),HL  ; 0 = нет петли
VGM_LOOP_DONE:
        ; VGM_END_ADDR и VGM_END_PAGE вычисляются в PLUGIN при загрузке файла

        ; Success
        POP BC
        POP DE
        POP HL
        XOR A                   ; Z = success
        RET

VGM_PARSE_FAIL:
        POP BC
        POP DE
        POP HL
        OR 1                    ; NZ = error
        RET

; ═══════════════════════════════════════════════════════════════════════════
; VGM_FILL_BUFFER
; ═══════════════════════════════════════════════════════════════════════════
; Fill a command buffer with ISR commands from VGM stream.
; Input: A = buffer index (0=BUF_A, 1=BUF_B)
; Reads from VGM_READ_PTR, updates it.
; ═══════════════════════════════════════════════════════════════════════════
VGM_FILL_BUFFER:
        PUSH HL
        PUSH DE
        PUSH BC
        PUSH IX

        ; If song ended or paused, fill with silence
        LD B,A                  ; save buffer index
        LD A,(SONG_ENDED)
        OR A
        JP NZ,VGM_FILL_SILENCE
        LD A,(PAUSED)
        OR A
        JP NZ,VGM_FILL_SILENCE
        LD A,B                  ; restore buffer index

        ; Get buffer base address
        OR A
        JR NZ,VGM_FILL_USE_B
        LD IX,CMD_BUF_A
        JR VGM_FILL_START
VGM_FILL_USE_B:
        LD IX,CMD_BUF_B
VGM_FILL_START:
        ; ВАЖНО: WC API (напр. UI_UPDATE) могла переключить страницу #C000!
        ; Восстановить правильную VPL-страницу перед чтением VGM.
        PUSH IX                 ; MNGCVPL (WLD) может затереть IX
        LD A,(VGM_CUR_PAGE)
        CALL MNGCVPL            ; отобразить нужную страницу VGM на #C000
        POP IX

        ; DE = write pointer (offset from buffer start)
        LD DE,0                 ; offset 0
        LD HL,(VGM_READ_PTR)    ; HL = VGM read pointer

VGM_FILL_LOOP:
        ; Check if buffer is almost full (leave room for END_BUF + 1 WAIT)
        ; Buffer = 512 bytes, reserve last 8 bytes
        ; When D>=1 offset>=256, close immediately
        ; When D=0, close if E >= 248 (#F8)
        LD A,D
        OR A
        JP NZ,VGM_FILL_CLOSE   ; offset >= 256, definitely close to end
        LD A,E
        CP #F8                  ; 248 - leave room in low page
        JP NC,VGM_FILL_CLOSE

        ; Проверка конца VGM: сравниваем CUR_PAGE с END_PAGE, затем HL с END_ADDR
        PUSH DE
        LD A,(VGM_CUR_PAGE)
        LD D,A
        LD A,(VGM_END_PAGE)
        CP D                    ; A=END_PAGE, D=CUR_PAGE
        JR C,VGM_FILL_AT_END   ; END_PAGE < CUR_PAGE → прошли конец
        JR NZ,VGM_FILL_NOT_END ; END_PAGE > CUR_PAGE → ещё не конец
        ; На последней странице: сравниваем HL с VGM_END_ADDR
        LD DE,(VGM_END_ADDR)
        LD A,H
        CP D
        JR C,VGM_FILL_NOT_END  ; H < D
        JR NZ,VGM_FILL_AT_END  ; H > D
        LD A,L
        CP E
        JR C,VGM_FILL_NOT_END  ; L < E
VGM_FILL_AT_END:
        POP DE
        ; Song ended
        LD A,1
        LD (SONG_ENDED),A
        JP VGM_FILL_CLOSE
VGM_FILL_NOT_END:
        POP DE

        ; Read next VGM opcode (+ проверка границы страницы)
        LD A,(HL)
        INC HL
        CALL VGM_CHK_PAGE

        ; === 0x5E: YMF262 port 0 write (addr, data) ===
        CP #5E
        JP Z,VGM_OP_OPL_PORT0

        ; === 0x5F: YMF262 port 1 write (addr, data) ===
        CP #5F
        JP Z,VGM_OP_OPL_PORT1

        ; === 0x61: Wait N samples (16-bit) ===
        CP #61
        JP Z,VGM_OP_WAIT_N

        ; === 0x62: Wait 735 samples (1/60 sec) ===
        CP #62
        JP Z,VGM_OP_WAIT_60HZ

        ; === 0x63: Wait 882 samples (1/50 sec) ===
        CP #63
        JP Z,VGM_OP_WAIT_50HZ

        ; === 0x66: End of sound data ===
        CP #66
        JP Z,VGM_OP_END

        ; === 0x70-0x7F: Wait 1-16 samples ===
        LD B,A
        AND #F0
        CP #70
        JP Z,VGM_OP_WAIT_SHORT

        ; Unknown opcode: try to skip it
        ; Most VGM opcodes are 1-3 bytes. Common skips:
        LD A,B                  ; restore opcode
        ; 0x3x, 0x4x - single byte (reserved)
        CP #50
        JP C,VGM_FILL_LOOP     ; < 0x50: skip 1 byte total (already read)
        ; 0x5x - 3 bytes total, we need to skip 2 more
        AND #F0
        CP #50
        JR Z,VGM_SKIP2
        ; 0xAx, 0xBx - 3 bytes
        LD A,B
        AND #F0
        CP #A0
        JR Z,VGM_SKIP2
        CP #B0
        JR Z,VGM_SKIP2
        ; 0xCx, 0xDx - 4 bytes
        CP #C0
        JR Z,VGM_SKIP3
        CP #D0
        JR Z,VGM_SKIP3
        ; 0xEx, 0xFx - 5 bytes
        CP #E0
        JR Z,VGM_SKIP4
        CP #F0
        JR Z,VGM_SKIP4
        ; Default: skip nothing more
        JP VGM_FILL_LOOP

VGM_SKIP2:
        INC HL
        CALL VGM_CHK_PAGE
        INC HL
        CALL VGM_CHK_PAGE
        JP VGM_FILL_LOOP
VGM_SKIP3:
        INC HL
        CALL VGM_CHK_PAGE
        INC HL
        CALL VGM_CHK_PAGE
        INC HL
        CALL VGM_CHK_PAGE
        JP VGM_FILL_LOOP
VGM_SKIP4:
        INC HL
        CALL VGM_CHK_PAGE
        INC HL
        CALL VGM_CHK_PAGE
        INC HL
        CALL VGM_CHK_PAGE
        INC HL
        CALL VGM_CHK_PAGE
        JP VGM_FILL_LOOP

; --- OPL3 port 0 write: emit WRITE addr0, WRITE data0 ---
VGM_OP_OPL_PORT0:
        ; Read register number and value
        LD B,(HL)               ; register number
        INC HL
        CALL VGM_CHK_PAGE
        LD C,(HL)               ; value
        INC HL
        CALL VGM_CHK_PAGE

        ; Emit: WRITE_B0 reg+val (банк 0)
        PUSH HL
        PUSH IX
        POP HL                  ; HL = buffer base
        ADD HL,DE               ; HL = current write position
        LD (HL),CMD_WRITE_B0    ; command
        INC HL
        LD (HL),B               ; register number
        INC HL
        LD (HL),C               ; value
        INC HL
        LD (HL),0               ; padding
        POP HL                  ; restore VGM read ptr

        ; Advance write offset by 4
        LD A,E
        ADD A,4
        LD E,A
        JP NC,VGM_FILL_LOOP
        INC D
        JP VGM_FILL_LOOP

; --- OPL3 port 1 write: emit WRITE addr1, WRITE data1 ---
VGM_OP_OPL_PORT1:
        LD B,(HL)               ; register number
        INC HL
        CALL VGM_CHK_PAGE
        LD C,(HL)               ; value
        INC HL
        CALL VGM_CHK_PAGE

        PUSH HL
        PUSH IX
        POP HL
        ADD HL,DE
        LD (HL),CMD_WRITE_B1    ; command
        INC HL
        LD (HL),B               ; register number (банк 1)
        INC HL
        LD (HL),C               ; value
        INC HL
        LD (HL),0               ; padding
        POP HL

        LD A,E
        ADD A,4
        LD E,A
        JP NC,VGM_FILL_LOOP
        INC D
        JP VGM_FILL_LOOP

; --- Wait N samples (16-bit LE) ---
; ISR freq = 2734 Hz, VGM samples = 44100 Hz. Factor = 44100/2734 ~ 16.13.
; Approximation: ISR_ticks = VGM_samples >> 4  (divide by 16)
; Ignore delays < 16 samples (too small to represent), emit nothing.
VGM_OP_WAIT_N:
        LD C,(HL)               ; ticks low
        INC HL
        CALL VGM_CHK_PAGE
        LD B,(HL)               ; ticks high
        INC HL
        CALL VGM_CHK_PAGE
        ; BC = N samples. Divide by 16: shift right 4.
        SRL B
        RR C
        SRL B
        RR C
        SRL B
        RR C
        SRL B
        RR C                    ; ISR_ticks in BC (= N/16)
        ; If BC == 0 (N < 16): skip
        LD A,B
        OR C
        JP Z,VGM_FILL_LOOP
        ; ISR_WAIT_CTR = ticks - 1
        DEC BC

        PUSH HL
        PUSH IX
        POP HL
        ADD HL,DE
        LD (HL),CMD_WAIT
        INC HL
        LD (HL),C               ; ticks low
        INC HL
        LD (HL),B               ; ticks high
        INC HL
        LD (HL),0               ; padding
        POP HL

        LD A,E
        ADD A,4
        LD E,A
        JP NC,VGM_FILL_LOOP
        INC D
        JP VGM_FILL_LOOP

; --- Wait 735 samples (1/60 sec) ---
VGM_OP_WAIT_60HZ:
        ; 735/16 = 45 ISR ticks, -1 = 44 = #2C
        PUSH HL
        PUSH IX
        POP HL
        ADD HL,DE
        LD (HL),CMD_WAIT
        INC HL
        LD (HL),#2C             ; 44 low
        INC HL
        LD (HL),#00             ; 44 high
        INC HL
        LD (HL),0
        POP HL

        LD A,E
        ADD A,4
        LD E,A
        JP NC,VGM_FILL_LOOP
        INC D
        JP VGM_FILL_LOOP

; --- Wait 882 samples (1/50 sec) ---
VGM_OP_WAIT_50HZ:
        ; 882/16 = 55 ISR ticks, -1 = 54 = #36
        PUSH HL
        PUSH IX
        POP HL
        ADD HL,DE
        LD (HL),CMD_WAIT
        INC HL
        LD (HL),#36             ; 54 low
        INC HL
        LD (HL),#00             ; 54 high
        INC HL
        LD (HL),0
        POP HL

        LD A,E
        ADD A,4
        LD E,A
        JP NC,VGM_FILL_LOOP
        INC D
        JP VGM_FILL_LOOP

; --- Wait 1-16 samples (opcode 0x70-0x7F) ---
; N = low nibble (0-15) = wait (N+1) samples.
; (N+1) >> 2: if < 1, skip; otherwise emit WAIT with value-1.
VGM_OP_WAIT_SHORT:
        LD A,B                  ; restore opcode
        AND #0F                 ; N = 0..15, actual wait = N+1 samples
        INC A                   ; A = 1..16 samples
        SRL A                   ; /2
        SRL A                   ; /4
        SRL A                   ; /8
        SRL A                   ; /16
        JP Z,VGM_FILL_LOOP     ; < 16 samples: skip entirely
        ; WAIT_CTR = A - 1
        DEC A
        LD C,A

        PUSH HL
        PUSH IX
        POP HL
        ADD HL,DE
        LD (HL),CMD_WAIT
        INC HL
        LD (HL),C               ; ticks low
        INC HL
        LD (HL),0               ; ticks high = 0
        INC HL
        LD (HL),0
        POP HL

        LD A,E
        ADD A,4
        LD E,A
        JP NC,VGM_FILL_LOOP
        INC D
        JP VGM_FILL_LOOP

; --- End of VGM data ---
VGM_OP_END:
        LD A,1
        LD (SONG_ENDED),A
        ; Fall through to close buffer

; --- Close buffer: emit CMD_END_BUF ---
VGM_FILL_CLOSE:
        LD (VGM_READ_PTR),HL    ; save VGM read position

        ; Write CMD_END_BUF at current write position
        PUSH IX
        POP HL
        ADD HL,DE
        LD (HL),CMD_END_BUF
        INC HL
        LD (HL),0
        INC HL
        LD (HL),0
        INC HL
        LD (HL),0

        POP IX
        POP BC
        POP DE
        POP HL
        RET

; --- Fill silence buffer (for paused/ended state) ---
VGM_FILL_SILENCE:
        ; Buffer index in B
        LD A,B
        OR A
        JR NZ,VGM_SIL_USE_B
        LD IX,CMD_BUF_A
        JR VGM_SIL_FILL
VGM_SIL_USE_B:
        LD IX,CMD_BUF_B
VGM_SIL_FILL:
        ; Fill with: WAIT 54 (~20ms at 2734Hz), END_BUF
        PUSH IX
        POP HL
        LD (HL),CMD_WAIT
        INC HL
        LD (HL),#36             ; 54 low (= ~20ms)
        INC HL
        LD (HL),#00             ; 54 high
        INC HL
        LD (HL),0
        INC HL
        LD (HL),CMD_END_BUF
        INC HL
        LD (HL),0
        INC HL
        LD (HL),0
        INC HL
        LD (HL),0

        POP IX
        POP BC
        POP DE
        POP HL
        RET

; ─── VGM_CHK_PAGE: проверка границы VPL-страницы после INC HL ─────────────
; Если HL = #0000 (переход #FFFF→#0000), отображает следующую VPL-страницу
; на #C000 и устанавливает HL = #C000.
; Сохраняет A (через PUSH/POP AF). Может изменить HL при смене страницы.
VGM_CHK_PAGE:
        PUSH AF
        LD A,H
        OR L
        JR NZ,VGM_CHK_RET
        ; Граница страницы: VGM_CUR_PAGE++, новая страница → #C000
        ; WC API (MNGCVPL) может затирать BC и DE — сохраняем их!
        PUSH BC
        PUSH DE
        PUSH IX                 ; WLD (WC API) затирает IX!
        LD A,(VGM_CUR_PAGE)
        INC A
        LD (VGM_CUR_PAGE),A
        CALL MNGCVPL            ; A = новый VPL-индекс → отобразить на #C000
        POP IX
        POP DE
        POP BC
        LD HL,#C000
VGM_CHK_RET:
        POP AF
        RET

; ═══════════════════════════════════════════════════════════════════════════
; VGM_FILL_INACTIVE_BUF (called from main loop)
; ═══════════════════════════════════════════════════════════════════════════
; Check which buffer ISR is reading, fill the other one if it changed.
; This is just a wrapper; main loop handles tracking in MAIN_LOOP.
; Kept for potential future use.

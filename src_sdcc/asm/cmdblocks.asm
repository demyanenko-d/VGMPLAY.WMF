; ============================================================================
; cmdblocks.asm — VGM-format command blocks for chip init/silence
;
; Compiled with sjasmplus → raw binary 16KB, appended as plugin page 5.
;
; Each block contains standard VGM opcodes identical to those in .vgm files:
;   0x5E rr vv  — OPL3 bank 0 write
;   0x5F rr vv  — OPL3 bank 1 write
;   0x55 rr vv  — YM2203 write (also used for MultiSound control regs 0xF0+)
;   0xA0 rr vv  — AY8910 write (bit7 of rr = chip 2)
;   0xBD rr vv  — SAA1099 write (bit7 of rr = chip 2)
;   0x66        — end of block
;
; The standard vgm_fill_buffer() parses these blocks in cmdblk_mode,
; converting VGM opcodes to ISR commands exactly as it does for .vgm data.
; This eliminates the need for a separate command copier — budget counting,
; buffer overflow protection and command parsing all come for free.
;
; NOTE on SAA clock:  MultiSound intercepts AY reg select 0xF0-0xFF on
;   port 0xFFFD for hardware control.  VGM opcode 0xA0 strips bit7 of
;   the register byte for chip selection, so 0xA0 CANNOT send regs >= 0x80
;   to chip 1.  We use YM2203 opcode 0x55 instead — it sends the register
;   byte raw to 0xFFFD, which MultiSound intercepts correctly.
;
; Layout:
;   0xC000 : pointer table (uint16 × N) — absolute addresses of blocks
;   0xC000+N*2.. : block data (variable-length VGM commands, 3 bytes each)
;
; Usage from C:
;   wc_mngc_pl(CMDBLK_PAGE);
;   vgm_read_ptr = ((uint16_t*)0xC000)[blk_idx];
;   vgm_cmdblk_mode = 1;  vgm_song_ended = 0;
;   vgm_fill_buffer(buf_idx);
;   vgm_cmdblk_mode = 0;
;
; Assembled with: sjasmplus asm\cmdblocks.asm
;                 → SAVEBIN "build/cmdblocks.bin", #C000, #4000
; ============================================================================

        DEVICE  NOSLOT64K
        ORG     #C000

; ── VGM opcodes ─────────────────────────────────────────────────────
VGM_OPL_B0   EQU #5E    ; OPL3 bank 0: 5E rr vv
VGM_OPL_B1   EQU #5F    ; OPL3 bank 1: 5F rr vv
VGM_YM2203   EQU #55    ; YM2203 SSG+FM: 55 rr vv
VGM_AY       EQU #A0    ; AY8910: A0 rr vv (bit7 = chip 2)
VGM_SAA      EQU #BD    ; SAA1099: BD rr vv (bit7 = chip 2)
VGM_END      EQU #66    ; End of data

; ── Macros (each emits standard VGM bytes, 3 bytes per command) ─────

    MACRO opl3_bank0 addr, val
        db VGM_OPL_B0, addr, val
    ENDM

    MACRO opl3_bank1 addr, val
        db VGM_OPL_B1, addr, val
    ENDM

    MACRO ym2203_write reg, val
        db VGM_YM2203, reg, val
    ENDM

    MACRO ay_write reg, val
        db VGM_AY, reg, val
    ENDM

    MACRO ay2_write reg, val
        db VGM_AY, reg | #80, val
    ENDM

    MACRO saa_write reg, val
        db VGM_SAA, reg, val
    ENDM

    MACRO saa2_write reg, val
        db VGM_SAA, reg | #80, val
    ENDM

    MACRO blk_end
        db VGM_END
    ENDM

; ═══════════════════════════════════════════════════════════════════════
; Pointer table (indices = CMDBLK_xxx from vgm.h)
; ═══════════════════════════════════════════════════════════════════════
ptr_table:
        dw blk_init_opl3           ; 0 = CMDBLK_INIT_OPL3
        dw blk_init_opl2           ; 1 = CMDBLK_INIT_OPL2
        dw blk_silence_opl         ; 2 = CMDBLK_SILENCE_OPL
        dw blk_silence_ay          ; 3 = CMDBLK_SILENCE_AY
        dw blk_silence_ay2         ; 4 = CMDBLK_SILENCE_AY2
        dw blk_silence_saa         ; 5 = CMDBLK_SILENCE_SAA
        dw blk_silence_saa2        ; 6 = CMDBLK_SILENCE_SAA2
        dw blk_saa_clock_on        ; 7 = CMDBLK_SAA_CLK_ON
        dw blk_saa_clock_off       ; 8 = CMDBLK_SAA_CLK_OFF
        dw blk_silence_ym2203      ; 9 = CMDBLK_SILENCE_YM2203
        dw blk_silence_ym2203_2    ; 10 = CMDBLK_SILENCE_YM2203_2

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_INIT_OPL3 — OPL3 init (NEW=1, clean state)
;
; OPL3 NEW=1 enables full OPL3 register set.  4-op connections are
; cleared (reg 0x104=0) to avoid leftover pairing from a previous
; track.  Test/WSE register 0x01 is zeroed — OPL3 does not use WSE;
; waveform select is implicit via E0-F5 registers.
;
; VGM stream:  5F 05 01  5F 04 00  5E 01 00  66
; ═══════════════════════════════════════════════════════════════════════
blk_init_opl3:
        opl3_bank1 #05, #01        ; VGM: 5F 05 01 — OPL3 NEW=1
        opl3_bank1 #04, #00        ; VGM: 5F 04 00 — 4-op OFF (clean)
        opl3_bank0 #01, #00        ; VGM: 5E 01 00 — Test/WSE=0
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_INIT_OPL2 — OPL2 compat init (NEW=0, clean state)
;
; Forces OPL2 compatibility mode.  Critically: 0x104=0 clears any
; residual 4-op state that would persist after an OPL3 track (the
; most common cause of OPL2 artifacts).  Test/WSE register = 0.
;
; VGM stream:  5F 05 00  5F 04 00  5E 01 00  66
; ═══════════════════════════════════════════════════════════════════════
blk_init_opl2:
        opl3_bank1 #05, #00        ; VGM: 5F 05 00 — OPL3 NEW=0 (OPL2)
        opl3_bank1 #04, #00        ; VGM: 5F 04 00 — 4-op OFF (clean)
        opl3_bank0 #01, #00        ; VGM: 5E 01 00 — Test/WSE=0
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_SILENCE_OPL — Full OPL3 silence (both banks)
;
; VGM stream (55 commands):
;   5E B0..B8 00   × 9    Key Off Bank 0 (ch 0-8)
;   5F B0..B8 00   × 9    Key Off Bank 1 (ch 0-8)
;   5E 4x 3F      ×18     TL max Bank 0 (18 operators)
;   5F 4x 3F      ×18     TL max Bank 1 (18 operators)
;   5E BD 00               Percussion Off
;   66                     End
; ═══════════════════════════════════════════════════════════════════════
blk_silence_opl:
        ; ── Key Off: Bank 0 — VGM: 5E B0..B8 00 ──
        opl3_bank0 #B0, #00
        opl3_bank0 #B1, #00
        opl3_bank0 #B2, #00
        opl3_bank0 #B3, #00
        opl3_bank0 #B4, #00
        opl3_bank0 #B5, #00
        opl3_bank0 #B6, #00
        opl3_bank0 #B7, #00
        opl3_bank0 #B8, #00
        ; ── Key Off: Bank 1 — VGM: 5F B0..B8 00 ──
        opl3_bank1 #B0, #00
        opl3_bank1 #B1, #00
        opl3_bank1 #B2, #00
        opl3_bank1 #B3, #00
        opl3_bank1 #B4, #00
        opl3_bank1 #B5, #00
        opl3_bank1 #B6, #00
        opl3_bank1 #B7, #00
        opl3_bank1 #B8, #00
        ; ── TL max: Bank 0 — VGM: 5E {40-45,48-4D,50-55} 3F ──
        opl3_bank0 #40, #3F
        opl3_bank0 #41, #3F
        opl3_bank0 #42, #3F
        opl3_bank0 #43, #3F
        opl3_bank0 #44, #3F
        opl3_bank0 #45, #3F
        opl3_bank0 #48, #3F
        opl3_bank0 #49, #3F
        opl3_bank0 #4A, #3F
        opl3_bank0 #4B, #3F
        opl3_bank0 #4C, #3F
        opl3_bank0 #4D, #3F
        opl3_bank0 #50, #3F
        opl3_bank0 #51, #3F
        opl3_bank0 #52, #3F
        opl3_bank0 #53, #3F
        opl3_bank0 #54, #3F
        opl3_bank0 #55, #3F
        ; ── TL max: Bank 1 — VGM: 5F {40-45,48-4D,50-55} 3F ──
        opl3_bank1 #40, #3F
        opl3_bank1 #41, #3F
        opl3_bank1 #42, #3F
        opl3_bank1 #43, #3F
        opl3_bank1 #44, #3F
        opl3_bank1 #45, #3F
        opl3_bank1 #48, #3F
        opl3_bank1 #49, #3F
        opl3_bank1 #4A, #3F
        opl3_bank1 #4B, #3F
        opl3_bank1 #4C, #3F
        opl3_bank1 #4D, #3F
        opl3_bank1 #50, #3F
        opl3_bank1 #51, #3F
        opl3_bank1 #52, #3F
        opl3_bank1 #53, #3F
        opl3_bank1 #54, #3F
        opl3_bank1 #55, #3F
        ; ── Waveform reset: Bank 0 — VGM: 5E {E0-E5,E8-ED,F0-F5} 00 ──
        ; Clears non-sine waveforms that linger between tracks.
        ; Critical for clean OPL2 playback after OPL3 usage.
        opl3_bank0 #E0, #00
        opl3_bank0 #E1, #00
        opl3_bank0 #E2, #00
        opl3_bank0 #E3, #00
        opl3_bank0 #E4, #00
        opl3_bank0 #E5, #00
        opl3_bank0 #E8, #00
        opl3_bank0 #E9, #00
        opl3_bank0 #EA, #00
        opl3_bank0 #EB, #00
        opl3_bank0 #EC, #00
        opl3_bank0 #ED, #00
        opl3_bank0 #F0, #00
        opl3_bank0 #F1, #00
        opl3_bank0 #F2, #00
        opl3_bank0 #F3, #00
        opl3_bank0 #F4, #00
        opl3_bank0 #F5, #00
        ; ── Waveform reset: Bank 1 — VGM: 5F {E0-E5,E8-ED,F0-F5} 00 ──
        opl3_bank1 #E0, #00
        opl3_bank1 #E1, #00
        opl3_bank1 #E2, #00
        opl3_bank1 #E3, #00
        opl3_bank1 #E4, #00
        opl3_bank1 #E5, #00
        opl3_bank1 #E8, #00
        opl3_bank1 #E9, #00
        opl3_bank1 #EA, #00
        opl3_bank1 #EB, #00
        opl3_bank1 #EC, #00
        opl3_bank1 #ED, #00
        opl3_bank1 #F0, #00
        opl3_bank1 #F1, #00
        opl3_bank1 #F2, #00
        opl3_bank1 #F3, #00
        opl3_bank1 #F4, #00
        opl3_bank1 #F5, #00
        ; ── Percussion Off — VGM: 5E BD 00 ──
        opl3_bank0 #BD, #00
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_SILENCE_AY — Silence AY8910 chip 1
;
; VGM stream:
;   A0 07 3F    Mixer: all tone+noise off
;   A0 08 00    Volume A = 0
;   A0 09 00    Volume B = 0
;   A0 0A 00    Volume C = 0
;   66          End
; ═══════════════════════════════════════════════════════════════════════
blk_silence_ay:
        ay_write 7, #3F            ; VGM: A0 07 3F
        ay_write 8, #00            ; VGM: A0 08 00
        ay_write 9, #00            ; VGM: A0 09 00
        ay_write 10, #00           ; VGM: A0 0A 00
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_SILENCE_AY2 — Silence AY8910 chip 2 (TurboSound)
;
; VGM stream (bit7 = chip 2):
;   A0 87 3F    Mixer off
;   A0 88 00    Volume A = 0
;   A0 89 00    Volume B = 0
;   A0 8A 00    Volume C = 0
;   66          End
; ═══════════════════════════════════════════════════════════════════════
blk_silence_ay2:
        ay2_write 7, #3F           ; VGM: A0 87 3F
        ay2_write 8, #00           ; VGM: A0 88 00
        ay2_write 9, #00           ; VGM: A0 89 00
        ay2_write 10, #00          ; VGM: A0 8A 00
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_SILENCE_SAA — Silence SAA1099 chip 1
;
; VGM stream:
;   BD 1C 02       Reset all (sound gen + internal state)
;   BD 1C 00       Sound enable off
;   BD 00..05 00   Amplitude ch 0-5 = 0
;   66             End
; ═══════════════════════════════════════════════════════════════════════
blk_silence_saa:
        saa_write #1C, #02         ; VGM: BD 1C 02 — Reset all
        saa_write #1C, #00         ; VGM: BD 1C 00 — Sound enable off
        saa_write #00, #00         ; VGM: BD 00 00 — Amp ch0 = 0
        saa_write #01, #00         ; VGM: BD 01 00 — Amp ch1 = 0
        saa_write #02, #00         ; VGM: BD 02 00 — Amp ch2 = 0
        saa_write #03, #00         ; VGM: BD 03 00 — Amp ch3 = 0
        saa_write #04, #00         ; VGM: BD 04 00 — Amp ch4 = 0
        saa_write #05, #00         ; VGM: BD 05 00 — Amp ch5 = 0
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_SILENCE_SAA2 — Silence SAA1099 chip 2
;
; VGM stream (bit7 = chip 2):
;   BD 9C 02       Reset all
;   BD 9C 00       Sound enable off
;   BD 80..85 00   Amplitude ch 0-5 = 0
;   66             End
; ═══════════════════════════════════════════════════════════════════════
blk_silence_saa2:
        saa2_write #1C, #02        ; VGM: BD 9C 02 — Reset all
        saa2_write #1C, #00        ; VGM: BD 9C 00 — Sound enable off
        saa2_write #00, #00        ; VGM: BD 80 00 — Amp ch0 = 0
        saa2_write #01, #00        ; VGM: BD 81 00 — Amp ch1 = 0
        saa2_write #02, #00        ; VGM: BD 82 00 — Amp ch2 = 0
        saa2_write #03, #00        ; VGM: BD 83 00 — Amp ch3 = 0
        saa2_write #04, #00        ; VGM: BD 84 00 — Amp ch4 = 0
        saa2_write #05, #00        ; VGM: BD 85 00 — Amp ch5 = 0
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_SAA_CLK_ON — Enable SAA1099 + FM clock (MultiSound)
;
; Uses YM2203 opcode (0x55) so that register 0xF3 is sent raw to 0xFFFD.
; AY opcode 0xA0 would strip bit7 for chip selection → can't reach 0xF3.
; MultiSound intercepts reg select 0xF0-0xFF for hardware control.
;   bit0=1(YM2) bit1=1(normal) bit2=0(FM ON) bit3=0(SAA ON)
;
; VGM stream:  55 F3 00  66
; ═══════════════════════════════════════════════════════════════════════
blk_saa_clock_on:
        ym2203_write #F3, #00      ; VGM: 55 F3 00 — SAA+FM clock ON
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_SAA_CLK_OFF — Disable SAA1099 + FM clock (MultiSound)
;
; VGM stream:  55 FF 00  66
;   bit2=1(FM OFF) bit3=1(SAA OFF)
; ═══════════════════════════════════════════════════════════════════════
blk_saa_clock_off:
        ym2203_write #FF, #00      ; VGM: 55 FF 00 — SAA+FM clock OFF
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_SILENCE_YM2203 — Silence YM2203 (SSG + FM)
;
; SSG (AY-compatible):
;   55 07 3F    Mixer: all tone+noise off
;   55 08 00    SSG Volume A = 0
;   55 09 00    SSG Volume B = 0
;   55 0A 00    SSG Volume C = 0
; FM — Key Off all 3 channels:
;   55 28 00    Key Off ch 0
;   55 28 01    Key Off ch 1
;   55 28 02    Key Off ch 2
; FM — TL max (0x7F) all 12 operators:
;   55 40..42 7F   slot 1 (M1): ch 0,1,2
;   55 44..46 7F   slot 3 (C1): ch 0,1,2
;   55 48..4A 7F   slot 2 (M2): ch 0,1,2
;   55 4C..4E 7F   slot 4 (C2): ch 0,1,2
; Total: 19 VGM commands + end = 58 bytes
; ═══════════════════════════════════════════════════════════════════════
blk_silence_ym2203:
        ; ── SSG silence — VGM: 55 rr vv ──
        ym2203_write 7, #3F        ; VGM: 55 07 3F — Mixer off
        ym2203_write 8, #00        ; VGM: 55 08 00 — SSG Vol A = 0
        ym2203_write 9, #00        ; VGM: 55 09 00 — SSG Vol B = 0
        ym2203_write 10, #00       ; VGM: 55 0A 00 — SSG Vol C = 0
        ; ── FM Key Off — VGM: 55 28 xx ──
        ym2203_write #28, #00      ; VGM: 55 28 00 — Key Off ch 0
        ym2203_write #28, #01      ; VGM: 55 28 01 — Key Off ch 1
        ym2203_write #28, #02      ; VGM: 55 28 02 — Key Off ch 2
        ; ── FM TL max — VGM: 55 4x 7F ──
        ym2203_write #40, #7F      ; VGM: 55 40 7F — TL slot1 ch0
        ym2203_write #41, #7F      ; VGM: 55 41 7F — TL slot1 ch1
        ym2203_write #42, #7F      ; VGM: 55 42 7F — TL slot1 ch2
        ym2203_write #44, #7F      ; VGM: 55 44 7F — TL slot3 ch0
        ym2203_write #45, #7F      ; VGM: 55 45 7F — TL slot3 ch1
        ym2203_write #46, #7F      ; VGM: 55 46 7F — TL slot3 ch2
        ym2203_write #48, #7F      ; VGM: 55 48 7F — TL slot2 ch0
        ym2203_write #49, #7F      ; VGM: 55 49 7F — TL slot2 ch1
        ym2203_write #4A, #7F      ; VGM: 55 4A 7F — TL slot2 ch2
        ym2203_write #4C, #7F      ; VGM: 55 4C 7F — TL slot4 ch0
        ym2203_write #4D, #7F      ; VGM: 55 4D 7F — TL slot4 ch1
        ym2203_write #4E, #7F      ; VGM: 55 4E 7F — TL slot4 ch2
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; CMDBLK_SILENCE_YM2203_2 — Silence YM2203 chip 2 (TurboSound)
;
; Uses AY opcode 0xA0 with bit7 set → vgm_fill_buffer generates
; CMD_WRITE_AY2 which selects chip 2 via TurboSound protocol.
;
; SSG (chip 2):
;   A0 87 3F    Mixer: all tone+noise off
;   A0 88 00    SSG Volume A = 0
;   A0 89 00    SSG Volume B = 0
;   A0 8A 00    SSG Volume C = 0
; FM — Key Off all 3 channels (chip 2):
;   A0 A8 00    Key Off ch 0
;   A0 A8 01    Key Off ch 1
;   A0 A8 02    Key Off ch 2
; FM — TL max (0x7F) all 12 operators (chip 2):
;   A0 C0..C2 7F   slot 1 (M1): ch 0,1,2
;   A0 C4..C6 7F   slot 3 (C1): ch 0,1,2
;   A0 C8..CA 7F   slot 2 (M2): ch 0,1,2
;   A0 CC..CE 7F   slot 4 (C2): ch 0,1,2
; Total: 19 VGM commands + end = 58 bytes
; ═══════════════════════════════════════════════════════════════════════
blk_silence_ym2203_2:
        ; ── SSG silence (chip 2) — VGM: A0 (reg|80) vv ──
        ay2_write 7, #3F            ; VGM: A0 87 3F — Mixer off
        ay2_write 8, #00            ; VGM: A0 88 00 — SSG Vol A = 0
        ay2_write 9, #00            ; VGM: A0 89 00 — SSG Vol B = 0
        ay2_write 10, #00           ; VGM: A0 8A 00 — SSG Vol C = 0
        ; ── FM Key Off (chip 2) — VGM: A0 (28|80) xx ──
        ay2_write #28, #00          ; VGM: A0 A8 00 — Key Off ch 0
        ay2_write #28, #01          ; VGM: A0 A8 01 — Key Off ch 1
        ay2_write #28, #02          ; VGM: A0 A8 02 — Key Off ch 2
        ; ── FM TL max (chip 2) — VGM: A0 (4x|80) 7F ──
        ay2_write #40, #7F          ; VGM: A0 C0 7F — TL slot1 ch0
        ay2_write #41, #7F          ; VGM: A0 C1 7F — TL slot1 ch1
        ay2_write #42, #7F          ; VGM: A0 C2 7F — TL slot1 ch2
        ay2_write #44, #7F          ; VGM: A0 C4 7F — TL slot3 ch0
        ay2_write #45, #7F          ; VGM: A0 C5 7F — TL slot3 ch1
        ay2_write #46, #7F          ; VGM: A0 C6 7F — TL slot3 ch2
        ay2_write #48, #7F          ; VGM: A0 C8 7F — TL slot2 ch0
        ay2_write #49, #7F          ; VGM: A0 C9 7F — TL slot2 ch1
        ay2_write #4A, #7F          ; VGM: A0 CA 7F — TL slot2 ch2
        ay2_write #4C, #7F          ; VGM: A0 CC 7F — TL slot4 ch0
        ay2_write #4D, #7F          ; VGM: A0 CD 7F — TL slot4 ch1
        ay2_write #4E, #7F          ; VGM: A0 CE 7F — TL slot4 ch2
        blk_end

; ═══════════════════════════════════════════════════════════════════════
; Padding to 16 KB
; ═══════════════════════════════════════════════════════════════════════
        IF $ < #10000
            ds #10000 - $, #00
        ENDIF

        SAVEBIN "build/cmdblocks.bin", #C000, #4000

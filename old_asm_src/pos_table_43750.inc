; Position table for 2734 Hz (step=1280, 56 entries, perfect 1-frame cycle)
; Format: [VSINTH, VSINTL, HSINT, next_lo, next_hi] - 5 bytes per entry
; Frequency: 3,500,000 / 1280 = 2734.375 Hz (~44100/16)
; At 14MHz with wait-states: ~3200 effective T-states between interrupts

POS_TABLE_ENTRIES EQU 56
ISR_FREQ          EQU 2734   ; used for SECOND_TIMER countdown

POS_TABLE:
        DB #00, #00, #00 : DW $+2  ; [  0] line=  0, hpos=  0
        DB #00, #05, #A0 : DW $+2  ; [  1] line=  5, hpos=160
        DB #00, #0B, #60 : DW $+2  ; [  2] line= 11, hpos= 96
        DB #00, #11, #20 : DW $+2  ; [  3] line= 17, hpos= 32
        DB #00, #16, #C0 : DW $+2  ; [  4] line= 22, hpos=192
        DB #00, #1C, #80 : DW $+2  ; [  5] line= 28, hpos=128
        DB #00, #22, #40 : DW $+2  ; [  6] line= 34, hpos= 64
        DB #00, #28, #00 : DW $+2  ; [  7] line= 40, hpos=  0
        DB #00, #2D, #A0 : DW $+2  ; [  8] line= 45, hpos=160
        DB #00, #33, #60 : DW $+2  ; [  9] line= 51, hpos= 96
        DB #00, #39, #20 : DW $+2  ; [ 10] line= 57, hpos= 32
        DB #00, #3E, #C0 : DW $+2  ; [ 11] line= 62, hpos=192
        DB #00, #44, #80 : DW $+2  ; [ 12] line= 68, hpos=128
        DB #00, #4A, #40 : DW $+2  ; [ 13] line= 74, hpos= 64
        DB #00, #50, #00 : DW $+2  ; [ 14] line= 80, hpos=  0
        DB #00, #55, #A0 : DW $+2  ; [ 15] line= 85, hpos=160
        DB #00, #5B, #60 : DW $+2  ; [ 16] line= 91, hpos= 96
        DB #00, #61, #20 : DW $+2  ; [ 17] line= 97, hpos= 32
        DB #00, #66, #C0 : DW $+2  ; [ 18] line=102, hpos=192
        DB #00, #6C, #80 : DW $+2  ; [ 19] line=108, hpos=128
        DB #00, #72, #40 : DW $+2  ; [ 20] line=114, hpos= 64
        DB #00, #78, #00 : DW $+2  ; [ 21] line=120, hpos=  0
        DB #00, #7D, #A0 : DW $+2  ; [ 22] line=125, hpos=160
        DB #00, #83, #60 : DW $+2  ; [ 23] line=131, hpos= 96
        DB #00, #89, #20 : DW $+2  ; [ 24] line=137, hpos= 32
        DB #00, #8E, #C0 : DW $+2  ; [ 25] line=142, hpos=192
        DB #00, #94, #80 : DW $+2  ; [ 26] line=148, hpos=128
        DB #00, #9A, #40 : DW $+2  ; [ 27] line=154, hpos= 64
        DB #00, #A0, #00 : DW $+2  ; [ 28] line=160, hpos=  0
        DB #00, #A5, #A0 : DW $+2  ; [ 29] line=165, hpos=160
        DB #00, #AB, #60 : DW $+2  ; [ 30] line=171, hpos= 96
        DB #00, #B1, #20 : DW $+2  ; [ 31] line=177, hpos= 32
        DB #00, #B6, #C0 : DW $+2  ; [ 32] line=182, hpos=192
        DB #00, #BC, #80 : DW $+2  ; [ 33] line=188, hpos=128
        DB #00, #C2, #40 : DW $+2  ; [ 34] line=194, hpos= 64
        DB #00, #C8, #00 : DW $+2  ; [ 35] line=200, hpos=  0
        DB #00, #CD, #A0 : DW $+2  ; [ 36] line=205, hpos=160
        DB #00, #D3, #60 : DW $+2  ; [ 37] line=211, hpos= 96
        DB #00, #D9, #20 : DW $+2  ; [ 38] line=217, hpos= 32
        DB #00, #DE, #C0 : DW $+2  ; [ 39] line=222, hpos=192
        DB #00, #E4, #80 : DW $+2  ; [ 40] line=228, hpos=128
        DB #00, #EA, #40 : DW $+2  ; [ 41] line=234, hpos= 64
        DB #00, #F0, #00 : DW $+2  ; [ 42] line=240, hpos=  0
        DB #00, #F5, #A0 : DW $+2  ; [ 43] line=245, hpos=160
        DB #00, #FB, #60 : DW $+2  ; [ 44] line=251, hpos= 96
        DB #01, #01, #20 : DW $+2  ; [ 45] line=257, hpos= 32
        DB #01, #06, #C0 : DW $+2  ; [ 46] line=262, hpos=192
        DB #01, #0C, #80 : DW $+2  ; [ 47] line=268, hpos=128
        DB #01, #12, #40 : DW $+2  ; [ 48] line=274, hpos= 64
        DB #01, #18, #00 : DW $+2  ; [ 49] line=280, hpos=  0
        DB #01, #1D, #A0 : DW $+2  ; [ 50] line=285, hpos=160
        DB #01, #23, #60 : DW $+2  ; [ 51] line=291, hpos= 96
        DB #01, #29, #20 : DW $+2  ; [ 52] line=297, hpos= 32
        DB #01, #2E, #C0 : DW $+2  ; [ 53] line=302, hpos=192
        DB #01, #34, #80 : DW $+2  ; [ 54] line=308, hpos=128
        DB #01, #3A, #40 : DW POS_TABLE  ; [ 55] line=314, hpos= 64
POS_TABLE_END:

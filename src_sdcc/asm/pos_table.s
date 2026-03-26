;============================================================================
; pos_table.s — Таблица позиций INT (СГЕНЕРИРОВАНО, не редактировать)
;
; entries=28, step=2560, line_len=224
; ISR freq: 3,500,000 / 2560 = 1367 Hz (~44100/16)
;============================================================================

        .module pos_table_mod
        .globl  pos_table

        .area _CODE

pos_table:
_pt_0:
        .db   0x00, 0x00, 0x00        ; [ 0] line=  0, hpos=  0
        .dw   _pt_1
_pt_1:
        .db   0x00, 0x0B, 0x60        ; [ 1] line= 11, hpos= 96
        .dw   _pt_2
_pt_2:
        .db   0x00, 0x16, 0xC0        ; [ 2] line= 22, hpos=192
        .dw   _pt_3
_pt_3:
        .db   0x00, 0x22, 0x40        ; [ 3] line= 34, hpos= 64
        .dw   _pt_4
_pt_4:
        .db   0x00, 0x2D, 0xA0        ; [ 4] line= 45, hpos=160
        .dw   _pt_5
_pt_5:
        .db   0x00, 0x39, 0x20        ; [ 5] line= 57, hpos= 32
        .dw   _pt_6
_pt_6:
        .db   0x00, 0x44, 0x80        ; [ 6] line= 68, hpos=128
        .dw   _pt_7
_pt_7:
        .db   0x00, 0x50, 0x00        ; [ 7] line= 80, hpos=  0
        .dw   _pt_8
_pt_8:
        .db   0x00, 0x5B, 0x60        ; [ 8] line= 91, hpos= 96
        .dw   _pt_9
_pt_9:
        .db   0x00, 0x66, 0xC0        ; [ 9] line=102, hpos=192
        .dw   _pt_10
_pt_10:
        .db   0x00, 0x72, 0x40        ; [10] line=114, hpos= 64
        .dw   _pt_11
_pt_11:
        .db   0x00, 0x7D, 0xA0        ; [11] line=125, hpos=160
        .dw   _pt_12
_pt_12:
        .db   0x00, 0x89, 0x20        ; [12] line=137, hpos= 32
        .dw   _pt_13
_pt_13:
        .db   0x00, 0x94, 0x80        ; [13] line=148, hpos=128
        .dw   _pt_14
_pt_14:
        .db   0x00, 0xA0, 0x00        ; [14] line=160, hpos=  0
        .dw   _pt_15
_pt_15:
        .db   0x00, 0xAB, 0x60        ; [15] line=171, hpos= 96
        .dw   _pt_16
_pt_16:
        .db   0x00, 0xB6, 0xC0        ; [16] line=182, hpos=192
        .dw   _pt_17
_pt_17:
        .db   0x00, 0xC2, 0x40        ; [17] line=194, hpos= 64
        .dw   _pt_18
_pt_18:
        .db   0x00, 0xCD, 0xA0        ; [18] line=205, hpos=160
        .dw   _pt_19
_pt_19:
        .db   0x00, 0xD9, 0x20        ; [19] line=217, hpos= 32
        .dw   _pt_20
_pt_20:
        .db   0x00, 0xE4, 0x80        ; [20] line=228, hpos=128
        .dw   _pt_21
_pt_21:
        .db   0x00, 0xF0, 0x00        ; [21] line=240, hpos=  0
        .dw   _pt_22
_pt_22:
        .db   0x00, 0xFB, 0x60        ; [22] line=251, hpos= 96
        .dw   _pt_23
_pt_23:
        .db   0x01, 0x06, 0xC0        ; [23] line=262, hpos=192
        .dw   _pt_24
_pt_24:
        .db   0x01, 0x12, 0x40        ; [24] line=274, hpos= 64
        .dw   _pt_25
_pt_25:
        .db   0x01, 0x1D, 0xA0        ; [25] line=285, hpos=160
        .dw   _pt_26
_pt_26:
        .db   0x01, 0x29, 0x20        ; [26] line=297, hpos= 32
        .dw   _pt_27
_pt_27:
        .db   0x01, 0x34, 0x80        ; [27] line=308, hpos=128
        .dw   pos_table

        .globl pos_table_end
pos_table_end:

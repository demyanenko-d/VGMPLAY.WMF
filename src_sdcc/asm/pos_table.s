;============================================================================
; pos_table.s — Таблица позиций INT (СГЕНЕРИРОВАНО, не редактировать)
;
; entries=14, step=5120, line_len=224
; ISR freq: 3,500,000 / 5120 = 684 Hz (~44100/16)
;============================================================================

        .module pos_table_mod
        .globl  pos_table

        .area _CODE

pos_table:
_pt_0:
        .db   0x00, 0x00, 0x00        ; [ 0] line=  0, hpos=  0
        .dw   _pt_1
_pt_1:
        .db   0x00, 0x16, 0xC0        ; [ 1] line= 22, hpos=192
        .dw   _pt_2
_pt_2:
        .db   0x00, 0x2D, 0xA0        ; [ 2] line= 45, hpos=160
        .dw   _pt_3
_pt_3:
        .db   0x00, 0x44, 0x80        ; [ 3] line= 68, hpos=128
        .dw   _pt_4
_pt_4:
        .db   0x00, 0x5B, 0x60        ; [ 4] line= 91, hpos= 96
        .dw   _pt_5
_pt_5:
        .db   0x00, 0x72, 0x40        ; [ 5] line=114, hpos= 64
        .dw   _pt_6
_pt_6:
        .db   0x00, 0x89, 0x20        ; [ 6] line=137, hpos= 32
        .dw   _pt_7
_pt_7:
        .db   0x00, 0xA0, 0x00        ; [ 7] line=160, hpos=  0
        .dw   _pt_8
_pt_8:
        .db   0x00, 0xB6, 0xC0        ; [ 8] line=182, hpos=192
        .dw   _pt_9
_pt_9:
        .db   0x00, 0xCD, 0xA0        ; [ 9] line=205, hpos=160
        .dw   _pt_10
_pt_10:
        .db   0x00, 0xE4, 0x80        ; [10] line=228, hpos=128
        .dw   _pt_11
_pt_11:
        .db   0x00, 0xFB, 0x60        ; [11] line=251, hpos= 96
        .dw   _pt_12
_pt_12:
        .db   0x01, 0x12, 0x40        ; [12] line=274, hpos= 64
        .dw   _pt_13
_pt_13:
        .db   0x01, 0x29, 0x20        ; [13] line=297, hpos= 32
        .dw   pos_table

        .globl pos_table_end
pos_table_end:

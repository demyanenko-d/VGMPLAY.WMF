;==============================================================================
; wc_api.s — Полные обёртки Wild Commander API для SDCC (sdcccall(1))
;
; ── Соглашение sdcccall(1) — SDCC 4.x Z80 ─────────────────────────────
;  Первый аргумент:  uint8  → A
;                    uint16 / ptr → HL  (ВАЖНО: HL, а не DE!)
;                    uint32 → DEHL
;  Второй аргумент:  uint16 / ptr → DE  (если 1-й ≤ 16 бит)
;                    uint8 →
;                        - если первый аргумент uint8: второй → L
;                        - иначе (если первый uint16/ptr): второй → НА СТЕК
;  Остальные:        стек правым аргументом первым
;                    uint8 = 1 байт на стеке, uint16 = 2 байта (без выравнивания!)
;                    callee обязан очистить стековые аргументы (callee-cleans)
;  Возврат:          uint8  → A,   uint16 → DE
;
; ── Стековый фрейм (типичный, внутри функции после PUSH IX, PUSH DE) ──
;   IX+0:  win_lo (сохранённый DE = win ptr, если PUSH DE производился)
;   IX+1:  win_hi
;   IX+2:  IX_save_lo
;   IX+3:  IX_save_hi
;   IX+4:  return address lo
;   IX+5:  return address hi
;   IX+6:  первый стековый аргумент (левый из оставшихся)
;   IX+8:  второй стековый аргумент
;   ... и т.д.  (каждый uint8 padding'ом до 2 байт)
;
; ── Трюк PUSH / POP для IX = регистр ──────────────────────────────────
;   PUSH HL; POP IX → IX = HL   (нет прямого LD IX, HL)
;
; ── Трюк EXX для сохранения BC/DE/HL в теневых регистрах ──────────────
;   EXX  — обменяет BC/DE/HL ↔ BC'/DE'/HL', не затрагивая IX и AF
;   EX AF,AF' — поменяет A/F ↔ A'/F' (для передачи A' в WC)
;
;==============================================================================

        .module wc_api

WC_ENTRY = 0x6006

        .area _CODE

;==================================================================
; СЕКЦИЯ 1: INIT / СТРАНИЧНАЯ КОММУТАЦИЯ
;==================================================================

;------------------------------------------------------------------
; void wc_mngc_pl(uint8_t page) — плагин-страница → #C000  (#00)
; sdcccall(1): A = page → A'
;------------------------------------------------------------------
        .globl _wc_mngc_pl
_wc_mngc_pl::
        ex      af, af'
        push    ix
        ld      a, #0x00
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_mng0_pl(uint8_t page) — плагин-страница → #0000  (#4E)
;------------------------------------------------------------------
        .globl _wc_mng0_pl
_wc_mng0_pl::
        ex      af, af'
        push    ix
        ld      a, #0x4E
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_mng8_pl(uint8_t page) — плагин-страница → #8000  (#4F)
;------------------------------------------------------------------
        .globl _wc_mng8_pl
_wc_mng8_pl::
        ex      af, af'
        push    ix
        ld      a, #0x4F
        call    WC_ENTRY
        pop     ix
        ret

;==================================================================
; СЕКЦИЯ 2: UI / ОКНА
;==================================================================

;------------------------------------------------------------------
; void wc_gedpl(void) — восстановить дисплей WC (#0F)
;------------------------------------------------------------------
        .globl _wc_gedpl
_wc_gedpl::
        push    ix
        ld      a, #0x0F
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_prwow(wc_window_t *win) — нарисовать окно (#01)
; sdcccall(1): HL = win → IX
;------------------------------------------------------------------
        .globl _wc_prwow
_wc_prwow::
        push    ix
        push    hl              ; IX = HL = win ptr
        pop     ix
        ld      a, #0x01
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_rresb(wc_window_t *win) — удалить окно (#02)
;------------------------------------------------------------------
        .globl _wc_rresb
_wc_rresb::
        push    ix
        push    hl              ; IX = HL = win ptr
        pop     ix
        ld      a, #0x02
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_cursor(wc_window_t *win) — нарисовать курсор (#06)
;------------------------------------------------------------------
        .globl _wc_cursor
_wc_cursor::
        push    ix
        push    hl              ; IX = HL = win ptr
        pop     ix
        ld      a, #0x06
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_curser(wc_window_t *win) — стереть курсор (#07)
;------------------------------------------------------------------
        .globl _wc_curser
_wc_curser::
        push    ix
        push    hl              ; IX = HL = win ptr
        pop     ix
        ld      a, #0x07
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_prsrw(win, str, y, x, len) — напечатать строку (#03)
; sdcccall(1): HL=win(1st ptr), DE=str(2nd ptr)
;              stack: y(1 byte), x(1 byte), len(2 bytes) = 4 bytes
; WC:  IX=win, HL=str, D=y, E=x, BC=len
; Callee cleans 4 bytes.
; Stack after push ix, push hl, push de:
;   0,1=str  2,3=win  4,5=IX  6,7=ret  8=y  9=x  10,11=len
;------------------------------------------------------------------
        .globl _wc_prsrw
_wc_prsrw::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        push    de                      ; save str (DE = 2nd arg)
        ld      ix, #0
        add     ix, sp
        ld      b, 11(ix)               ; B = len_hi
        ld      c, 10(ix)               ; C = len_lo
        ld      d, 8(ix)                ; D = y
        ld      e, 9(ix)                ; E = x
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = str
        exx
        ld      l, 2(ix)
        ld      h, 3(ix)                ; HL = win
        push    hl
        pop     ix                      ; IX = win
        exx                             ; BC=len, DE=y|x, HL=str
        ld      a, #0x03
        call    WC_ENTRY
        pop     de                      ; remove saved str
        pop     hl                      ; remove saved win
        pop     ix                      ; restore caller IX
        ; callee cleans: y(1)+x(1)+len(2) = 4 bytes
        pop     hl                      ; HL = return address
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        jp      (hl)

;------------------------------------------------------------------
; void wc_prsrw_attr(win, str, y, x, len, attr) — напечатать строку + атрибут (#03 + #04)
; sdcccall(1): HL=win(1st ptr), DE=str(2nd ptr)
;              stack: y(1), x(1), len(2), attr(1) = 5 bytes
; WC #03: IX=win, HL=str, D=y, E=x, BC=len
; WC #04: IX=win, A'=attr, D=y, E=x, BC=len
; Callee cleans 5 bytes.
; Stack after push ix, push hl, push de:
;   0,1=str  2,3=win  4,5=IX  6,7=ret  8=y  9=x  10=len_lo  11=len_hi  12=attr
;------------------------------------------------------------------
        .globl _wc_prsrw_attr
_wc_prsrw_attr::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        push    de                      ; save str (DE = 2nd arg)
        ld      ix, #0
        add     ix, sp

        ; ── вызов #03 (prsrw) ──────────────────────────────────
        ld      b, 11(ix)               ; B = len_hi
        ld      c, 10(ix)               ; C = len_lo
        ld      d, 8(ix)                ; D = y
        ld      e, 9(ix)                ; E = x
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = str
        exx
        ld      l, 2(ix)
        ld      h, 3(ix)                ; HL = win
        ld      a, 12(ix)               ; A = attr (IX всё ещё → фрейм)
        push    af                      ; сохраняем attr на стеке
        push    hl
        pop     ix                      ; IX = win
        exx                             ; BC=len, DE=y|x, HL=str; HL'=win
        ld      a, #0x03
        call    WC_ENTRY
        ; после возврата: BC/DE=len/y|x (WC #03 не портит),
        ;                  HL'=win (WC сохраняет alternate regs)

        ; ── вызов #04 (priat) ──────────────────────────────────
        pop     af                      ; A = attr
        ex      af, af'                 ; A' = attr
        exx                             ; HL = win (из HL'), BC=len, DE=y|x
        push    hl
        pop     ix                      ; IX = win
        exx                             ; restore BC=len, DE=y|x
        ld      a, #0x04
        call    WC_ENTRY

        pop     de                      ; remove saved str
        pop     hl                      ; remove saved win
        pop     ix                      ; restore caller IX
        ; callee cleans 5 bytes: y(1)+x(1)+len(2)+attr(1)
        pop     hl                      ; HL = return address
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        jp      (hl)

;------------------------------------------------------------------
; uint16_t wc_gadrw(win, y, x) — адрес ячейки экрана (#05)
; sdcccall(1): HL=win(1st ptr), stack: y(1 byte), x(1 byte) = 2 bytes
; WC: IX=win, D=y, E=x → HL=адрес
; sdcccall(1) returns uint16_t in DE.
; Callee cleans 2 bytes.
; Stack after push ix, push hl: 0,1=win 2,3=IX 4,5=ret 6=y 7=x
;------------------------------------------------------------------
        .globl _wc_gadrw
_wc_gadrw::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        ld      ix, #0
        add     ix, sp
        ld      d, 6(ix)                ; D = y
        ld      e, 7(ix)                ; E = x
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = win
        push    hl
        pop     ix                      ; IX = win
        ld      a, #0x05
        call    WC_ENTRY                ; HL = result addr
        ex      de, hl                  ; DE = result (sdcccall(1) uint16)
        pop     hl                      ; remove saved win (use HL, keep DE)
        pop     ix                      ; restore caller IX
        ; callee cleans 2 bytes (y+x); preserve DE = return value
        pop     hl                      ; HL = return address
        inc     sp
        inc     sp                      ; discard y(1)+x(1)
        jp      (hl)                    ; return, DE = result

;------------------------------------------------------------------
; uint8_t wc_yn(uint8_t mode) — меню Да/Нет (#08)
; sdcccall(1): A=mode → A'
;------------------------------------------------------------------
        .globl _wc_yn
_wc_yn::
        ex      af, af'
        push    ix
        ld      a, #0x08
        call    WC_ENTRY
        ; Возвращаем: Z=ok → A=1, NZ → A=0
        ld      a, #0
        jr      nz, _wc_yn_end
        inc     a
_wc_yn_end:
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_istr(wc_window_t *win, uint8_t mode) — редактор строки (#09)
; sdcccall(1): HL=win(1st ptr), stack: mode(1 byte)
; WC: IX=win, A'=mode
; Callee cleans 1 byte.
;------------------------------------------------------------------
        .globl _wc_istr
_wc_istr::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      a, 4(ix)                ; A = mode (from stack)
        ex      af, af'                 ; A' = mode
        push    hl
        pop     ix                      ; IX = win (HL = 1st arg)
        ld      a, #0x09
        call    WC_ENTRY
        pop     ix
        ; callee cleans 1 byte (mode)
        pop     hl
        inc     sp
        jp      (hl)

;------------------------------------------------------------------
; void wc_nork(uint16_t addr, uint8_t val) — вывести HEX байт (#0A)
; sdcccall(1): HL=addr(1st uint16), stack: val(1 byte)
; WC: HL=addr, A'=val
; Callee cleans 1 byte.
;------------------------------------------------------------------
        .globl _wc_nork
_wc_nork::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      a, 4(ix)                ; A = val (from stack)
        ex      af, af'                 ; A' = val
        ; HL = addr (1st arg, already in HL; WC needs HL)
        ld      a, #0x0A
        call    WC_ENTRY
        pop     ix
        ; callee cleans 1 byte (val)
        pop     hl
        inc     sp
        jp      (hl)

;------------------------------------------------------------------
; uint8_t wc_txtpr(win, str, y, x) — печать текста (#0B)
; sdcccall(1): HL=win, DE=str; стек: push bc где B=y, C=x
; WC: IX=win, HL=str, D=y, E=x → D=след. строка
; Callee cleans 2 stack bytes (push bc).
; Stack from IX after push ix, push hl, push de:
;   IX+0,1=str  IX+2,3=win  IX+4,5=IX  IX+6,7=ret  IX+8=C=x  IX+9=B=y
;------------------------------------------------------------------
        .globl _wc_txtpr
_wc_txtpr::
        push    ix              ; (1) save caller IX
        push    hl              ; (2) save win ptr  (HL = 1st arg)
        push    de              ; (3) save str ptr  (DE = 2nd arg)
        ld      ix, #0
        add     ix, sp
        ld      d, 9(ix)        ; D = y  (B from caller's push bc, hi byte)
        ld      e, 8(ix)        ; E = x  (C from caller's push bc, lo byte)
        ld      l, 0(ix)
        ld      h, 1(ix)        ; HL = str ptr
        exx                     ; shadow: HL'=str, D'=y, E'=x
        ld      l, 2(ix)
        ld      h, 3(ix)        ; HL = win ptr
        push    hl
        pop     ix              ; IX = win ptr
        exx                     ; restore HL=str, D=y, E=x
        ld      a, #0x0B
        call    WC_ENTRY
        ld      a, d            ; A = return: next Y
        pop     de              ; (3) restore DE
        pop     hl              ; (2) restore HL
        pop     ix              ; (1) restore IX
        pop     hl              ; HL = return address
        inc     sp              ; } discard caller's push bc (2 bytes: y, x)
        inc     sp              ; }
        jp      (hl)            ; return (callee cleans stack args)

;------------------------------------------------------------------
; uint8_t wc_mezz(win, msg_num, str, y, x) — сообщение WC (#0C)
; sdcccall(1): HL=win(1st ptr), stack: msg_num(1), str(2), y(1), x(1) = 5 bytes
; WC: IX=win, A'=msg_num, HL=str, D=y, E=x → D=след. строка
; Callee cleans 5 bytes.
; Stack after push ix, push hl:
;   0,1=win 2,3=IX 4,5=ret 6=msg_num 7,8=str 9=y 10=x
;------------------------------------------------------------------
        .globl _wc_mezz
_wc_mezz::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        ld      ix, #0
        add     ix, sp
        ld      a, 6(ix)                ; A = msg_num
        ld      l, 7(ix)
        ld      h, 8(ix)                ; HL = str
        ld      d, 9(ix)                ; D = y
        ld      e, 10(ix)               ; E = x
        exx
        ex      af, af'                 ; A' = msg_num
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = win
        push    hl
        pop     ix                      ; IX = win
        exx                             ; HL=str, D=y, E=x
        ld      a, #0x0C
        call    WC_ENTRY
        ld      a, d                    ; return: next row
        pop     hl                      ; remove saved win
        pop     ix
        ; callee cleans 5 bytes
        pop     hl                      ; HL = return address
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        jp      (hl)

;------------------------------------------------------------------
; void wc_scrlwow(win, y, x, h, w, flags) — прокрутка (#54)
; sdcccall(1): HL=win(1st ptr), stack: y(1),x(1),h(1),w(1),flags(1) = 5 bytes
; WC: IX=win, D=y, E=x, B=h, C=w, A'=flags
; Callee cleans 5 bytes.
; Stack after push ix, push hl:
;   0,1=win 2,3=IX 4,5=ret 6=y 7=x 8=h 9=w 10=flags
;------------------------------------------------------------------
        .globl _wc_scrlwow
_wc_scrlwow::
        push    ix
        push    hl                      ; save win (HL = 1st arg)
        ld      ix, #0
        add     ix, sp
        ld      a, 10(ix)               ; A = flags
        ld      b, 8(ix)                ; B = h
        ld      c, 9(ix)                ; C = w
        ld      d, 6(ix)                ; D = y
        ld      e, 7(ix)                ; E = x
        exx
        ex      af, af'                 ; A' = flags
        ld      l, 0(ix)
        ld      h, 1(ix)
        push    hl
        pop     ix                      ; IX = win
        exx                             ; BC=h/w, DE=y/x
        ld      a, #0x54
        call    WC_ENTRY
        pop     hl                      ; remove saved win
        pop     ix
        ; callee cleans 5 bytes
        pop     hl
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        inc     sp
        jp      (hl)
        pop     de
        pop     ix
        ret

;==================================================================
; СЕКЦИЯ 3: КЛАВИАТУРА
; Шаблон для клавиш без аргументов:
;   push ix; ld a,#N; call WC_ENTRY; pop ix; ret
;==================================================================

;------------------------------------------------------------------
; Макро-генерация функций проверки клавиш (push ix не нужен для WC,
; но сохраняем на случай, если WC его трогает в редких случаях)
;------------------------------------------------------------------

        .globl _wc_key_space
_wc_key_space::
        push    ix
        ld      a, #0x10
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_up
_wc_key_up::
        push    ix
        ld      a, #0x11
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_down
_wc_key_down::
        push    ix
        ld      a, #0x12
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_left
_wc_key_left::
        push    ix
        ld      a, #0x13
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_right
_wc_key_right::
        push    ix
        ld      a, #0x14
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_tab
_wc_key_tab::
        push    ix
        ld      a, #0x15
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_enter
_wc_key_enter::
        push    ix
        ld      a, #0x16
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_esc
_wc_key_esc::
        push    ix
        ld      a, #0x17
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_bspc
_wc_key_bspc::
        push    ix
        ld      a, #0x18
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_pgup
_wc_key_pgup::
        push    ix
        ld      a, #0x19
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_pgdn
_wc_key_pgdn::
        push    ix
        ld      a, #0x1A
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_home
_wc_key_home::
        push    ix
        ld      a, #0x1B
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_end
_wc_key_end::
        push    ix
        ld      a, #0x1C
        call    WC_ENTRY
        pop     ix
        ret

; F1–F10 (#1D–#26)
        .globl _wc_key_f1
_wc_key_f1::
        push    ix
        ld      a, #0x1D
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_f2
_wc_key_f2::
        push    ix
        ld      a, #0x1E
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_f3
_wc_key_f3::
        push    ix
        ld      a, #0x1F
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_f4
_wc_key_f4::
        push    ix
        ld      a, #0x20
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_f5
_wc_key_f5::
        push    ix
        ld      a, #0x21
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_f6
_wc_key_f6::
        push    ix
        ld      a, #0x22
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_f7
_wc_key_f7::
        push    ix
        ld      a, #0x23
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_f8
_wc_key_f8::
        push    ix
        ld      a, #0x24
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_f9
_wc_key_f9::
        push    ix
        ld      a, #0x25
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_f10
_wc_key_f10::
        push    ix
        ld      a, #0x26
        call    WC_ENTRY
        pop     ix
        ret

; Alt/Shift/Ctrl (без автоповтора)
        .globl _wc_key_alt
_wc_key_alt::
        push    ix
        ld      a, #0x27
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_shift
_wc_key_shift::
        push    ix
        ld      a, #0x28
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_ctrl
_wc_key_ctrl::
        push    ix
        ld      a, #0x29
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; uint8_t wc_kbscn(uint8_t mode) — сканировать клавишу (#2A)
; sdcccall(1): A=mode → A'
; Возврат: A=keycode (0=нет)
;------------------------------------------------------------------
        .globl _wc_kbscn
_wc_kbscn::
        ex      af, af'                 ; A' = mode
        push    ix
        ld      a, #0x2A
        call    WC_ENTRY
        pop     ix
        ret

; Del/Caps/AnyKey/WaitRelease/WaitAny/Ins
        .globl _wc_key_del
_wc_key_del::
        push    ix
        ld      a, #0x2B
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_caps
_wc_key_caps::
        push    ix
        ld      a, #0x2C
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_any
_wc_key_any::
        push    ix
        ld      a, #0x2D
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_wait_release
_wc_key_wait_release::
        push    ix
        ld      a, #0x2E
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_wait_any
_wc_key_wait_any::
        push    ix
        ld      a, #0x2F
        call    WC_ENTRY
        pop     ix
        ret

        .globl _wc_key_ins
_wc_key_ins::
        push    ix
        ld      a, #0x55
        call    WC_ENTRY
        pop     ix
        ret

;==================================================================
; СЕКЦИЯ 4: ФАЙЛОВЫЕ ОПЕРАЦИИ
;==================================================================

;------------------------------------------------------------------
; uint8_t wc_load512(uint16_t dest, uint8_t blocks) — загрузить (#30)
; sdcccall(1): HL=dest; стек: blocks (1 байт, push af + inc sp)
; WC: HL=dest, B=blocks → A=0 ok / 0x0F EOF
; Callee cleans 1 stack byte (blocks).
; Stack после push ix: IX+2=ret, IX+4=blocks
;------------------------------------------------------------------
        .globl _wc_load512
_wc_load512::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      b, 4(ix)                ; B = blocks (1 byte on stack)
        ; HL = dest (sdcccall(1) first uint16 → HL — do NOT ex de,hl!)
        ld      a, #0x30
        call    WC_ENTRY
        pop     ix
        pop     hl                      ; HL = return address
        inc     sp                      ; discard 1-byte 'blocks' arg
        jp      (hl)                    ; return (callee cleans stack arg)

;------------------------------------------------------------------
; void wc_save512(uint16_t src, uint8_t blocks) — сохранить (#31)
; sdcccall(1): HL=src; стек: blocks (1 байт, callee cleans)
;------------------------------------------------------------------
        .globl _wc_save512
_wc_save512::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      b, 4(ix)
        ; HL = src (DON'T ex de, hl!)
        ld      a, #0x31
        call    WC_ENTRY
        pop     ix
        pop     hl
        inc     sp
        jp      (hl)

;------------------------------------------------------------------
; void wc_gipagpl(void) — позиционировать на начало файла (#32)
;------------------------------------------------------------------
        .globl _wc_gipagpl
_wc_gipagpl::
        push    ix
        ld      a, #0x32
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_tentry(uint16_t addr) — прочитать ENTRY в буфер (#33)
; sdcccall(1): HL=addr (1st uint16 → HL)
; WC: DE=addr
;------------------------------------------------------------------
        .globl _wc_tentry
_wc_tentry::
        push    ix
        ex      de, hl                  ; DE = addr (SDCC HL → WC DE)
        ld      a, #0x33
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_chtosep(uint16_t buf, uint16_t bufend)  (#34)
; sdcccall(1): HL=buf(1st uint16), DE=bufend(2nd uint16)
; WC: DE=buf, BC=bufend
; No stack args.
;------------------------------------------------------------------
        .globl _wc_chtosep
_wc_chtosep::
        push    ix
        ld      b, d
        ld      c, e                    ; BC = bufend (from DE)
        ex      de, hl                  ; DE = buf    (from HL)
        ld      a, #0x34
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_tmrkdfl(panel, filenum, namebuf) — заголовок помеч. файла (#36)
; sdcccall(1): HL=panel(1st ptr), DE=filenum(2nd uint16)
;              stack: namebuf(2 bytes)
; WC: IX=panel, HL=filenum, DE=namebuf
; Callee cleans 2 bytes.
; Stack after push ix, push hl, push de:
;   0,1=filenum 2,3=panel 4,5=IX 6,7=ret 8,9=namebuf
;------------------------------------------------------------------
        .globl _wc_tmrkdfl
_wc_tmrkdfl::
        push    ix
        push    hl                      ; save panel (HL = 1st arg)
        push    de                      ; save filenum (DE = 2nd arg)
        ld      ix, #0
        add     ix, sp
        ld      e, 8(ix)
        ld      d, 9(ix)                ; DE = namebuf (from stack)
        ld      l, 0(ix)
        ld      h, 1(ix)                ; HL = filenum (from saved DE)
        exx
        ld      l, 2(ix)
        ld      h, 3(ix)                ; HL = panel (from saved HL)
        push    hl
        pop     ix                      ; IX = panel
        exx                             ; HL=filenum, DE=namebuf
        ld      a, #0x36
        call    WC_ENTRY
        pop     de                      ; remove saved filenum
        pop     hl                      ; remove saved panel
        pop     ix
        ; callee cleans 2 bytes (namebuf)
        pop     hl
        inc     sp
        inc     sp
        jp      (hl)

;------------------------------------------------------------------
; void wc_adir(uint8_t mode) — позиционирование директории (#38)
; sdcccall(1): A=mode → A'
;------------------------------------------------------------------
        .globl _wc_adir
_wc_adir::
        ex      af, af'
        push    ix
        ld      a, #0x38
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_stream(uint8_t mode) — переключить поток (#39)
; sdcccall(1): A=mode
; WC: D=mode
;------------------------------------------------------------------
        .globl _wc_stream
_wc_stream::
        push    ix
        ld      d, a
        ld      a, #0x39
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; uint8_t wc_findnext(uint16_t entry_buf, uint8_t flags) — FindNext (#3A)
; sdcccall(1): HL=entry_buf(1st uint16), stack: flags(1 byte)
; WC: DE=entry_buf, A'=flags → Z=конец, NZ=найден
; Callee cleans 1 byte.
;------------------------------------------------------------------
        .globl _wc_findnext
_wc_findnext::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      a, 4(ix)                ; A = flags (from stack)
        ex      af, af'                 ; A' = flags
        ex      de, hl                  ; DE = entry_buf (SDCC HL → WC DE)
        ld      a, #0x3A
        call    WC_ENTRY
        ld      a, #0
        jr      z, _wc_findnext_end
        inc     a
_wc_findnext_end:
        pop     ix
        ; callee cleans 1 byte (flags)
        pop     hl
        inc     sp
        jp      (hl)

;------------------------------------------------------------------
; uint8_t wc_fentry(uint16_t name_with_flag) — найти файл (#3B)
; sdcccall(1): HL=name (1st uint16 → HL)
; WC: HL=name → Z=не найден, NZ=найден
;------------------------------------------------------------------
        .globl _wc_fentry
_wc_fentry::
        push    ix
        ; HL = name (sdcccall(1); WC нужен HL — уже на месте!)
        ld      a, #0x3B
        call    WC_ENTRY
        ld      a, #0
        jr      z, _wc_fentry_end
        inc     a
_wc_fentry_end:
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_loadnone(uint8_t sectors) — пропустить секторы (#3D)
; sdcccall(1): A = sectors
; WC: B=sectors (предположительно)
;------------------------------------------------------------------
        .globl _wc_loadnone
_wc_loadnone::
        push    ix
        ld      b, a
        ld      a, #0x3D
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_gfile(void) — открыть найденный файл (#3E)
;------------------------------------------------------------------
        .globl _wc_gfile
_wc_gfile::
        push    ix
        ld      a, #0x3E
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_gdir(void) — войти в найденный каталог (#3F)
;------------------------------------------------------------------
        .globl _wc_gdir
_wc_gdir::
        push    ix
        ld      a, #0x3F
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; uint8_t wc_mkfile(uint16_t name_with_flag) — создать файл (#48)
; sdcccall(1): HL=name (1st uint16 → HL)
; WC: HL=name → A=ошибка (0=ok)
;------------------------------------------------------------------
        .globl _wc_mkfile
_wc_mkfile::
        push    ix
        ld      a, #0x48
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; uint8_t wc_mkdir(uint16_t name) — создать каталог (#49)
; sdcccall(1): HL=name; WC: HL=name
;------------------------------------------------------------------
        .globl _wc_mkdir
_wc_mkdir::
        push    ix
        ld      a, #0x49
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; uint8_t wc_rename(uint16_t old_name, uint16_t new_name)  (#4A)
; sdcccall(1): HL=old_name(1st), DE=new_name(2nd)
; WC: HL=old_name, DE=new_name  (уже на месте!)
; No stack args.
;------------------------------------------------------------------
        .globl _wc_rename
_wc_rename::
        push    ix
        ld      a, #0x4A
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; uint8_t wc_delfl(uint16_t name_with_flag) — удалить файл (#4B)
; sdcccall(1): HL=name; WC: HL=name
;------------------------------------------------------------------
        .globl _wc_delfl
_wc_delfl::
        push    ix
        ld      a, #0x4B
        call    WC_ENTRY
        pop     ix
        ret

;==================================================================
; СЕКЦИЯ 5: ГРАФИКА
;==================================================================

;------------------------------------------------------------------
; void wc_mngv_pl(uint8_t mode) — видеорежим WC (#40)
; A=mode → A'
;------------------------------------------------------------------
        .globl _wc_mngv_pl
_wc_mngv_pl::
        ex      af, af'
        push    ix
        ld      a, #0x40
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_mngcvpl(uint8_t vpage) — видеостраница → #C000 (#41)
; A=vpage → A'
;------------------------------------------------------------------
        .globl _wc_mngcvpl
_wc_mngcvpl::
        ex      af, af'
        push    ix
        ld      a, #0x41
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_gvmod(uint8_t mode) — установить видеорежим TSConfig (#42)
; A=mode → A'
;------------------------------------------------------------------
        .globl _wc_gvmod
_wc_gvmod::
        ex      af, af'
        push    ix
        ld      a, #0x42
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_gyoff(uint16_t y) — Y-смещение прокрутки (#43)
; sdcccall(1): HL=y (1st uint16 → HL)
; WC: HL=y  (уже на месте!)
;------------------------------------------------------------------
        .globl _wc_gyoff
_wc_gyoff::
        push    ix
        ld      a, #0x43
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_gxoff(uint16_t x) — X-смещение прокрутки (#44)
; sdcccall(1): HL=x; WC: HL=x
;------------------------------------------------------------------
        .globl _wc_gxoff
_wc_gxoff::
        push    ix
        ld      a, #0x44
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_gvtm(uint8_t page) — страница тайловой карты (#45)
; WC: C=page
;------------------------------------------------------------------
        .globl _wc_gvtm
_wc_gvtm::
        push    ix
        ld      c, a
        ld      a, #0x45
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_gvtl(uint8_t plane, uint8_t page) — тайл-графика (#46)
; sdcccall(1): A=plane(1st uint8), stack: page(1 byte)
; WC: B=plane, C=page
; Callee cleans 1 byte.
;------------------------------------------------------------------
        .globl _wc_gvtl
_wc_gvtl::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      c, 4(ix)                ; C = page
        ld      b, a                    ; B = plane
        ld      a, #0x46
        call    WC_ENTRY
        pop     ix
        ; callee cleans 1 byte (page)
        pop     hl
        inc     sp
        jp      (hl)

;------------------------------------------------------------------
; void wc_gvsgp(uint8_t page) — страница спрайт-графики (#47)
; WC: C=page
;------------------------------------------------------------------
        .globl _wc_gvsgp
_wc_gvsgp::
        push    ix
        ld      c, a
        ld      a, #0x47
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_mng0vpl(uint8_t vpage) — видеостраница → #0000 (#50)
;------------------------------------------------------------------
        .globl _wc_mng0vpl
_wc_mng0vpl::
        ex      af, af'
        push    ix
        ld      a, #0x50
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_mng8vpl(uint8_t vpage) — видеостраница → #8000 (#51)
;------------------------------------------------------------------
        .globl _wc_mng8vpl
_wc_mng8vpl::
        ex      af, af'
        push    ix
        ld      a, #0x51
        call    WC_ENTRY
        pop     ix
        ret

;==================================================================
; СЕКЦИЯ 6: DMA
;==================================================================

;------------------------------------------------------------------
; void wc_dmapl(uint8_t subfunc) — операции DMA (#0D)
; A=subfunc → A'
;------------------------------------------------------------------
        .globl _wc_dmapl
_wc_dmapl::
        ex      af, af'
        push    ix
        ld      a, #0x0D
        call    WC_ENTRY
        pop     ix
        ret

;==================================================================
; СЕКЦИЯ 7: РАЗНОЕ
;==================================================================

;------------------------------------------------------------------
; void wc_turbopl(uint8_t mode, uint8_t param) — частота CPU/AY (#0E)
; sdcccall(1): A=mode(1st uint8), L=param(2nd uint8)
; WC: B=mode, C=param
; Нет стековых аргументов.
;------------------------------------------------------------------
        .globl _wc_turbopl
_wc_turbopl::
        push    ix
        ld      c, l                    ; C = param (2-й аргумент)
        ld      b, a                    ; B = mode (1-й аргумент)
        ld      a, #0x0E
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; uint8_t wc_prm_pl(uint8_t param_num) — параметр из INI (#53)
; A=param_num → A'
; Возврат: 0xFF = нет параметра, иное = номер опции
;------------------------------------------------------------------
        .globl _wc_prm_pl
_wc_prm_pl::
        ex      af, af'
        push    ix
        ld      a, #0x53
        call    WC_ENTRY
        ; Z=нет параметра → вернуть 0xFF, иначе A=номер опции
        jr      nz, _wc_prm_pl_end
        ld      a, #0xFF
_wc_prm_pl_end:
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_int_pl(uint8_t mode) — управление прерываниями WC (#56)
; A=mode → A'  (режимы: WC_INT_*)
;------------------------------------------------------------------
        .globl _wc_int_pl
_wc_int_pl::
        ex      af, af'
        push    ix
        ld      a, #0x56
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; void wc_int_pl_handler(uint16_t handler_addr) — установить ISR (#56)
; sdcccall(1): HL=handler_addr (1st uint16 → HL)
; WC: A'=0xFF, HL=addr  (HL уже на месте!)
;------------------------------------------------------------------
        .globl _wc_int_pl_handler
_wc_int_pl_handler::
        push    ix
        ; HL = handler_addr (sdcccall(1); WC тоже нужен HL)
        ld      a, #0xFF
        ex      af, af'                 ; A' = 0xFF
        ld      a, #0x56
        call    WC_ENTRY
        pop     ix
        ret

;------------------------------------------------------------------
; uint8_t wc_load256(uint16_t dest, uint8_t blocks) — загрузить 256б блоков (#3C)
; sdcccall(1): HL=dest; стек: blocks (1 байт, callee cleans)
; WC: HL=addr, B=blocks → A=EndOfChain(#0F)
;------------------------------------------------------------------
        .globl _wc_load256
_wc_load256::
        push    ix
        ld      ix, #0
        add     ix, sp
        ld      b, 4(ix)                ; B = blocks (1 byte on stack)
        ; HL = dest (already in HL)
        ld      a, #0x3C
        call    WC_ENTRY
        pop     ix
        pop     hl                      ; HL = return address
        inc     sp                      ; discard 1-byte 'blocks' arg
        jp      (hl)                    ; return (callee cleans stack arg)

;------------------------------------------------------------------
; void wc_resident_jump(uint8_t page, uint16_t addr) — переход в другую страницу
; sdcccall(1): A=page, DE=addr (uint16 2nd → DE)
;              Нет stack args.
; Реализация: LD HL,DE; LD A,page; JP #6020
;------------------------------------------------------------------
        .globl _wc_resident_jump
_wc_resident_jump::
        ex      de, hl                  ; HL = addr (from DE = 2nd arg)
        ; A = page (1st arg, already in A)
        jp      0x6020                  ; resident jump — never returns!

;------------------------------------------------------------------
; void wc_resident_call(uint8_t page, uint16_t addr) — вызов в другой странице
; sdcccall(1): A=page, DE=addr (uint16 2nd → DE)
;              Нет stack args.
; Реализация: LD HL,DE; LD A,page; CALL #6028
;------------------------------------------------------------------
        .globl _wc_resident_call
_wc_resident_call::
        ex      de, hl                  ; HL = addr (from DE = 2nd arg)
        ; A = page (1st arg, already in A)
        call    0x6028                  ; resident call — returns here
        ret

;------------------------------------------------------------------
; void wc_strset(char *dsr, uint16_t len, char c)
; Заполняет строку символом c на длину len
; sdcccall(1): HL=dst(1st ptr), DE=len(2nd uint16),
;              stack: c(1 byte)
; Callee cleans 1 byte
;-------------------------------------------------------------------
       .globl _wc_strset
_wc_strset::
        push    ix
        ld      ix, #0
        add     ix, sp

        ld      a, 4(ix)     ; c (из-за push ix)
        pop     ix

        ; записываем первый байт
        ld      (hl), a

        ld      b, d
        ld      c, e

        ; len == 1 → всё
        dec     bc
        ld      a, b
        or      c
        jr      z, done

        ; DE = HL + 1
        push    hl
        pop     de
        inc     de

        ; BC = len - 1 уже

        ldir

done:
        ; очистка стека
        pop     bc
        inc     sp
        push    bc
        ret

;==============================================================================
; inflate_call.s — SDCC обёртка (sdcccall(1)) для вызова inflate из C
;
; uint8_t inflate_vgz(uint8_t src_page, uint8_t dst_page);
; void    copy_pages_to_tap(uint8_t n_pages);
;
;   sdcccall(1):   A = первый аргумент
;   Возврат inflate_vgz: A = 0 (успех) / 1 (ошибка)
;
; Inflate binary живёт в экстра-странице WMF.  Обёртка маппит её в Win 3
; (ORG 0xC000) и делает JP 0xC000.  Decode buffer — в Win 2 (0x8000),
; настраивается самим inflate.  При выходе inflate восстанавливает Win 2.
;
; ╔══════════════════════════════════════════════════════════════════════╗
; ║ Почему код inflate обязан быть в Win 3, а не Win 2:                ║
; ║ DEFLATE backref вычисляет HL = write_ptr − distance.               ║
; ║ При decode buffer в Win 2 (0x8000) → HL ∈ [0x0000..0xBFFF],       ║
; ║ никогда не попадает в Win 3 (0xC000+), где живёт код.              ║
; ║ Если поменять местами (код Win 2, decode Win 3) → HL может         ║
; ║ попасть в 0x8000–0xBFFF, что потребует ремапа Win 2 во время       ║
; ║ исполнения из неё — невозможно.                                    ║
; ╚══════════════════════════════════════════════════════════════════════╝
;
; copy_pages_to_tap:
;   Копирует N страниц из мегабуфера (#20+) в TAP-страницы (#A1+).
;   DI на время копирования, восстанавливает Win 0/Win 3 из sys vars.
;
; inflate_vgz:
;   1. Сохранить аргументы, IX, SP, Win 1
;   2. DI, маппить Win 3 = inflate page через порт
;   3. SP = 0xFFF0, push return addr → JP 0xC000
;   4. По возврату: SP, Win 1 восстанавливаем, EI, IX → ret
;   Win 0 / Win 3 — восстанавливает вызывающий C-код через WC API
;==============================================================================

        .module inflate_call

        .globl _inflate_vgz
        .globl _copy_pages_to_tap

;--- Константы ---------------------------------------------------------------
INFLATE_PAGE_IDX  = 4           ; индекс inflate в плагине (1 code + 3 LUT)
                                ; физ. страница = base_page + INFLATE_PAGE_IDX
                                ; base_page читается из WC sys var (0x6002)
PORT_W0           = 0x10AF      ; TSConfig Win 0 page port
PORT_W1           = 0x11AF      ; TSConfig Win 1 page port
PORT_W3           = 0x13AF      ; TSConfig Win 3 page port

;--- DATA (0xB800+) ----------------------------------------------------------
        .area _DATA

_ifl_src_pg:    .ds 1           ; src_page  (физ. стр. TAP)
_ifl_dst_pg:    .ds 1           ; dst_page  (физ. стр. мегабуфера)
_ifl_sv_pg1:    .ds 1           ; saved Win 1 physical page
_ifl_sv_pg2:    .ds 1           ; saved Win 2 physical page → inflate D reg
_ifl_sv_sp:     .ds 2           ; saved SP
_ifl_inflate_pg: .ds 1          ; computed inflate phys page (debug/scan)

;--- CODE (0x8000+) ----------------------------------------------------------
        .area _CODE

_inflate_vgz::
        push    ix                      ; callee-saved (SDCC)

        ;--- 1. сохранить аргументы ---
        ld      (_ifl_src_pg), a        ; A  = src_page
        ld      a, l
        ld      (_ifl_dst_pg), a        ; L  = dst_page

        ;--- 2. DI ---
        di

        ;--- 3. сохранить Win 1, Win 2 из WC sys vars (@ 0x6001/0x6002) ---
        ld      a, (0x6001)
        ld      (_ifl_sv_pg1), a
        ld      a, (0x6002)
        ld      (_ifl_sv_pg2), a

        ;--- 4. сохранить SP ---
        ld      (_ifl_sv_sp), sp

.ifdef DBG_ENABLE
        ;--- DBG: 0x01 = saved context, about to map inflate page ---
        ld      bc, #0xFFAF
        ld      a, #0x01
        out     (c), a
.endif

        ;--- 5. маппить Win 3 = inflate page (dynamic: base + IDX) ---
        ld      a, (0x6002)             ; Win 2 phys page = plugin base
.ifdef DBG_ENABLE
        ld      bc, #0xFEAF
        out     (c), a              ; DBG: FEAF = base page from 0x6002
.endif
        add     a, #INFLATE_PAGE_IDX    ; + 5 = inflate page
        ld      (_ifl_inflate_pg), a    ; save computed inflate page
.ifdef DBG_ENABLE
        ld      bc, #0xFDAF
        out     (c), a              ; DBG: FDAF = computed inflate page
.endif
        ld      bc, #PORT_W3
        out     (c), a

.ifdef DBG_ENABLE
        ;--- DBG: 0x02 = Win3 mapped to inflate page ---
        ld      bc, #0xFFAF
        ld      a, #0x02
        out     (c), a
        ld      a, (0xC000)
        ld      bc, #0xFEAF
        out     (c), a              ; byte[0] — expect 0xF5 (push af)
        ld      a, (0xC001)
        ld      bc, #0xFDAF
        out     (c), a              ; byte[1] — expect 0xC5 (push bc)

        ;=== DBG: scan pages base+3..base+7 looking for inflate ===
        ld      a, (0x6002)
        add     a, #3
        ld      d, a
        ld      e, #0
_ifl_scan_loop:
        ld      a, d
        ld      bc, #PORT_W3
        out     (c), a
        ld      a, #0xD0
        add     a, e
        ld      bc, #0xFFAF
        out     (c), a
        ld      a, d
        ld      bc, #0xFDAF
        out     (c), a
        ld      a, (0xC000)
        ld      bc, #0xFEAF
        out     (c), a
        inc     d
        inc     e
        ld      a, e
        cp      #5
        jr      nz, _ifl_scan_loop

        ;--- DBG: 0x04 = scan done, re-map Win3 to inflate page ---
        ld      bc, #0xFFAF
        ld      a, #0x04
        out     (c), a
        ld      a, (_ifl_inflate_pg)
        ld      bc, #PORT_W3
        out     (c), a
.endif

        ;--- 6. стек в Win 3, push return addr ---
        ld      sp, #0xFFF0
        ld      hl, #_ifl_return
        push    hl

.ifdef DBG_ENABLE
        ;--- DBG: 0x05 = SP set, return addr pushed, loading regs ---
        ld      bc, #0xFFAF
        ld      a, #0x05
        out     (c), a
        ld      a, (_ifl_src_pg)
        ld      bc, #0xFEAF
        out     (c), a
        ld      a, (_ifl_dst_pg)
        ld      bc, #0xFDAF
        out     (c), a
.endif

        ;--- 7. входные регистры inflate: A=src, E=dst, D=saved_pg2 ---
        ld      a, (_ifl_sv_pg2)
        ld      d, a                    ; D = saved Win 2 page
        ld      a, (_ifl_dst_pg)
        ld      e, a                    ; E = dst_page
        ld      a, (_ifl_src_pg)        ; A = src_page

.ifdef DBG_ENABLE
        ;--- DBG: 0x06 = about to JP 0xC000 ---
        push    af
        push    bc
        ld      bc, #0xFFAF
        ld      a, #0x06
        out     (c), a
        pop     bc
        pop     af
.endif

        ;--- 8. запуск inflate ---
        ;    JP 0xC200 = inflate_init (safe zone, survives backref LDIR)
        ;    НЕ JP 0xC000! Entry zone 0xC000–0xC1FF перезаписывается
        ;    LDIR overflow при распаковке backref'ов.
        jp      0xC200

        ;--- сюда вернёмся по RET из inflate ---
        ;    A = 0 (ok) / 1 (error)
        ;    Win 2 уже восстановлен inflate → _DATA и код доступны
_ifl_return:
        ld      e, a                    ; результат → E

.ifdef DBG_ENABLE
        ;--- DBG: 0x03 = returned from inflate, A=result ---
        ld      bc, #0xFFAF
        ld      a, #0x03
        out     (c), a
        ld      a, e
        ld      bc, #0xFEAF
        out     (c), a
.endif

        ;--- восстановить SP (→ C-стек, сразу после push ix) ---
        ld      sp, (_ifl_sv_sp)

        ;--- восстановить Win 1 (WC API точка входа @ 0x6006) ---
        ld      a, (_ifl_sv_pg1)
        ld      bc, #PORT_W1
        out     (c), a

        ;--- вернуть результат ---
        ld      a, e
        pop     ix                      ; восстановить IX
        ei                              ; EI + задержка 1 instr → ret атомарен
        ret


;==============================================================================
; void copy_pages_to_tap(uint8_t n_pages)
;
; Копирует n_pages страниц из мегабуфера (#20..#20+n-1) в TAP (#A1..#A1+n-1).
; Использует прямые port-записи в Win 0 / Win 3.
; DI на время работы; при выходе: Win 0, Win 3 восстановлены, EI.
;
; sdcccall(1): A = n_pages
;==============================================================================

MEGABUF_START = 0x20            ; первая физ. страница мегабуфера
TAP_START     = 0xA1            ; первая физ. страница TAP

_copy_pages_to_tap::
        or      a
        ret     z                       ; n=0 → ничего делать

        push    ix

        ld      c, a                    ; C = n_pages (сохраним)
        di

        ;--- сохранить текущие страницы Win 0 / Win 3 ---
        ld      a, (0x6000)
        ld      (_cpt_sv_pg0), a
        ld      a, (0x6003)
        ld      (_cpt_sv_pg3), a

        ;--- подготовить счётчики ---
        ld      d, #MEGABUF_START       ; D = src phys page
        ld      e, #TAP_START           ; E = dst phys page
        ld      b, c                    ; B = loop counter

_cpt_loop:
        push    bc
        push    de

        ;--- Map Win 0 = megabuf page D ---
        ld      a, d
        ld      bc, #PORT_W0
        out     (c), a

        ;--- Map Win 3 = TAP page E ---
        ld      a, e
        ld      b, #0x13                ; C ещё 0xAF от предыдущего BC
        out     (c), a

        ;--- LDIR 16 КБ: Win 0 → Win 3 ---
        ld      hl, #0x0000
        ld      de, #0xC000
        ld      bc, #0x4000
        ldir

        pop     de
        inc     d                       ; следующая megabuf page
        inc     e                       ; следующая TAP page
        pop     bc
        djnz    _cpt_loop

        ;--- восстановить Win 0 ---
        ld      a, (_cpt_sv_pg0)
        ld      bc, #PORT_W0
        out     (c), a

        ;--- восстановить Win 3 ---
        ld      a, (_cpt_sv_pg3)
        ld      b, #0x13
        out     (c), a

        ei
        pop     ix
        ret

;--- static vars for copy (в CODE — не в DATA, чтобы не зависеть от Win 2) ---
_cpt_sv_pg0:    .ds 1
_cpt_sv_pg3:    .ds 1

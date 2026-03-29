;==============================================================================
; @file   isr.s
; @brief  ISR FRAME 2734 Hz + инициализация IM2 (на SDCC Z80)
;
; Частота: 3,500,000 / 1280 = 2734.375 Hz (~44100/16).
; 56 INT x 1280 T = 71680 T = 1 TV-кадр (идеальное замыкание).
;
; Таблица позиций (pos_table.s): 56 x 5 байт = 280 байт.
; Формат записи: [VSINTH, VSINTL, HSINT, next_lo, next_hi]
; Читается тремя OUTI: BC=#24AF -> VSINTH, #23AF -> VSINTL, #22AF -> HSINT,
; затем читается next_ptr.
;
; Командные буферы: cmd_buf_a, cmd_buf_b (по 512 байт, 128 команд).
;
; Переменные ISR (volatile, доступны из C через isr.h):
;   _isr_active_buf   : 0=A, 1=B  (пишет ISR, читает main)
;   _isr_enabled      : 0/1       (пишет main, читает ISR)
;   _isr_play_seconds : uint16    (счётчик секунд, пишет ISR)
;   _isr_read_ptr     : uint16    (указатель чтения в активном буфере)
;
; Расход памяти _DATA:
;   _im2_table_buf    : 512 байт (гарантирует 256-aligned)
;   _cmd_buf_a/b      : 2 x 512 = 1024 байт
;   Переменные            : 14 байт
;   Итого               : ~1550 байт
;==============================================================================

        .module isr_mod

        .globl  _isr_active_buf
        .globl  _isr_enabled
        .globl  _isr_tick_ctr
        .globl  _isr_border_color
        .globl  _isr_play_seconds
        .globl  _isr_read_ptr
        .globl  _isr_done
        .globl  _cmd_buf_a
        .globl  _cmd_buf_b
        .globl  _isr_init
        .globl  _isr_deinit
        .globl  _call_wc_handler

; Импорт таблицы позиций из pos_table.s
        .globl  pos_table

; Порты TSConfig
PORT_HSINT  = 0x22AF
PORT_VSINTL = 0x23AF
PORT_VSINTH = 0x24AF
PORT_INTMASK = 0x2AAF

; Адрес вектора FRAME INT в IM2-таблице WC (I=#5B, vector=0xFF → #5BFF)
WC_IM2_VEC = 0x5BFF

; Коды команд (8 типов, шаг 0x20 + ISR_DONE)
CMD_WRITE_AY  = 0x00
CMD_INC_SEC   = 0x10
CMD_WRITE_AY2 = 0x20
CMD_CALL_WC   = 0x30
CMD_WRITE_B0  = 0x40
CMD_SKIP_TICKS= 0x50
CMD_WRITE_SAA = 0x60
CMD_WRITE_B1  = 0x80
CMD_WRITE_SAA2= 0xA0
CMD_WAIT      = 0xC0
CMD_END_BUF   = 0xE0
CMD_ISR_DONE  = 0xF0

; Порты OPL3
OPL3_ADDR0 = 0xC4
OPL3_DATA0 = 0xC5
OPL3_ADDR1 = 0xC6
OPL3_DATA1 = 0xC7

; Порты AY-3-8910 / YM2149
AY_ADDR    = 0xFD       ; C-byte, B=0xFF → #FFFD
AY_DATA_B  = 0xBF       ; B-byte, C=#FD  → #BFFD

; Порты SAA1099 (MultiSound card)
;   SAA1:  addr=#00FF  data=#01FF
;   SAA2:  addr=#02FF  data=#03FF

ISR_FREQ = 683    ; informational only (actual timing from pos_table)

;==============================================================================
; isr_init() — Перехват вектора IM2 WC и настройка 683 Hz
;==============================================================================
        .area _CODE

;==============================================================================
; Вместо собственной IM2-таблицы (512 байт) используем существующую
; таблицу WC.  WC работает в IM2 с I=#5B; вектор FRAME (data bus=0xFF)
; читается из адреса #5BFF (2 байта: lo, hi).
;
; isr_init:
;   1. Сохранить текущий вектор (#5BFF) → _isr_saved_vec
;   2. Записать адрес _isr_handler в #5BFF
;   3. Настроить INTMASK, позицию INT, указатели
;   4. IM 2 (для надёжности)
;   Вызывающий код делает EI.
;
; isr_deinit:
;   1. Восстановить сохранённый вектор WC
;   2. Сбросить позицию INT на line=0 (1 INT/кадр)
;   Вызывающий код делает EI.
;==============================================================================
_isr_init::
        di

        ;--- Сохранить оригинальный вектор WC ---
        ld      hl, (WC_IM2_VEC)
        ld      (_isr_saved_vec), hl

        ;--- Установить наш обработчик ---
        ld      hl, #_isr_handler
        ld      (WC_IM2_VEC), hl

        ;--- Разрешить только FRAME INT ---
        ld      bc, #PORT_INTMASK
        ld      a, #0x01
        out     (c), a

        ;--- Инициализировать указатель позиции ---
        ld      hl, #pos_table
        ld      (_pos_ptr), hl

        ;--- Инициализировать счётчик ожидания = 0 ---
        ld      hl, #0
        ld      (_isr_wait_ctr), hl

        ;--- Инициализировать read_ptr → cmd_buf_a ---
        ld      hl, #_cmd_buf_a
        ld      (_isr_read_ptr), hl

        ;--- Счётчик тиков ---
        xor     a
        ld      (_isr_tick_ctr), a

        ;--- Счётчик секунд ---
        ld      hl, #0
        ld      (_isr_play_seconds), hl

        ;--- Начальная позиция: строка 0, hpos 0 ---
        ld      bc, #PORT_HSINT
        out     (c), a
        ld      b, #>PORT_VSINTL
        out     (c), a
        ld      b, #>PORT_VSINTH
        out     (c), a

        ;--- Подтвердить IM2 (WC и так в IM2, но для надёжности) ---
        im      2
        ret

;==============================================================================
; isr_deinit() — Восстановить вектор WC и сбросить INT-позиции
;==============================================================================
_isr_deinit::
        di

        ;--- Проверить: вызывался ли isr_init? ---
        ld      hl, (_isr_saved_vec)
        ld      a, h
        or      l
        ret     z               ; saved_vec == 0 → init не вызывался

        ;--- Восстановить оригинальный вектор WC ---
        ld      (WC_IM2_VEC), hl

        ;--- Сбросить saved_vec (защита от повторного вызова) ---
        ld      hl, #0
        ld      (_isr_saved_vec), hl

        ;--- Сбросить позицию INT: line=0, hpos=0 (один INT/кадр) ---
        ld      bc, #PORT_HSINT
        xor     a
        out     (c), a
        ld      b, #>PORT_VSINTL
        out     (c), a
        ld      b, #>PORT_VSINTH
        out     (c), a

        ;--- INTMASK = FRAME only (WC по умолчанию) ---
        ld      bc, #PORT_INTMASK
        ld      a, #0x01
        out     (c), a

        ; Не делаем EI — вызывающий код решает сам
        ret

;==============================================================================
; ISR_HANDLER — обработчик прерывания FRAME (2734 Гц)
; ─── OUTI-блок и таблицу НЕ ТРОГАТЬ! ────────────────────────────────────
;==============================================================================
        .area _CODE

_isr_handler::
        push    af
        ld      a, #0x06            ; DEBUG: yellow border
        out     (0xFE), a
        push    hl
        push    bc
        push    de

        ;--- OUTI×3: записать следующую позицию INT из таблицы ---
        ; Формат записи: [vsinth_byte, vsintl_byte, hsint_byte, next_lo, next_hi]
        ; BC стартует 0x24AF: OUTI→0x24AF(VSINTH), B→0x23; OUTI→0x23AF(VSINTL), B→0x22; OUTI→0x22AF(HSINT)
        ld      hl, (_pos_ptr)
        ld      bc, #0x25AF         ; B=#25, C=#AF
        outi                        ; → VSINTH (#24AF)  outi predecrement B: B=0x24, C=0xAF
        outi                        ; → VSINTL (#23AF)
        outi                        ; → HSINT  (#22AF)
        ; Следующий ptr (2 байта)
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a                ; HL = next_ptr
        ld      (_pos_ptr), hl

        ;--- Счётчик тиков: простой INC byte (21T, всегда) ---
        ld      hl, #_isr_tick_ctr
        inc     (hl)                ; 11T: wrap at 256 — main loop считает секунды

        ;--- WAIT_CTR: если 0 — выполнять команды ---
        ld      a, (_isr_enabled)
        or      a
        jp      z, _isr_exit        ; не активен — быстрый выход
        ld      hl, (_isr_wait_ctr)
        ld      a, h
        or      l
        jp      z, _isr_exec_cmd
        dec     hl
        ld      (_isr_wait_ctr), hl

_isr_exit:
        ld      a, (_isr_border_color) ; DEBUG: restore border from variable
        out     (0xFE), a
        pop     de
        pop     bc
        pop     hl
        pop     af
        ei
        ret

;--- Выполнение команд --------------------------------------------------
_isr_exec_cmd:
        ld      hl, (_isr_read_ptr)
        ex      de, hl              ; DE = указатель команд

_isr_cmd_loop:
        ld      a, (de)
        inc     de
        ; Dispatch: 8 command types at 0x20 boundaries
        ; Hot path first: OPL writes (most common for OPL files)
        cp      #CMD_WRITE_B0
        jp      z, _isr_write_b0
        cp      #CMD_WRITE_B1
        jp      z, _isr_write_b1
        cp      #CMD_WAIT
        jp      z, _isr_do_wait
        ; AY/SAA/end commands (less frequent per-interrupt)
        cp      #CMD_WRITE_AY
        jp      z, _isr_write_ay
        cp      #CMD_WRITE_AY2
        jp      z, _isr_write_ay2
        cp      #CMD_WRITE_SAA
        jp      z, _isr_write_saa
        cp      #CMD_WRITE_SAA2
        jp      z, _isr_write_saa2
        ; Rare commands (before END_BUF fallthrough)
        cp      #CMD_END_BUF
        jp      z, _isr_do_end_buf
        cp      #CMD_ISR_DONE
        jp      z, _isr_cmd_done
        cp      #CMD_INC_SEC
        jp      z, _isr_inc_sec
        cp      #CMD_CALL_WC
        jp      z, _isr_call_wc
        cp      #CMD_SKIP_TICKS
        jp      z, _isr_skip_ticks
        ; Unknown command: skip 3 bytes, continue
        inc     de
        inc     de
        inc     de
        jp      _isr_cmd_loop

_isr_do_end_buf:
        ld      a, (_isr_active_buf)
        xor     #1
        ld      (_isr_active_buf), a
        jr      nz, _isr_end_use_b
        ld      hl, #_cmd_buf_a
        jr      _isr_end_save_ptr
_isr_end_use_b:
        ld      hl, #_cmd_buf_b
_isr_end_save_ptr:
        ld      (_isr_read_ptr), hl
        ex      de, hl
        jp      _isr_cmd_loop

_isr_write_b0:
        ld      a, (de)             ; reg
        ld      c, #OPL3_ADDR0      ; B don't-care for C4-C7
        out     (c), a
        inc     de
        ld      a, (de)             ; val
        inc     c                   ; C5 = data port
        out     (c), a
        inc     de
        inc     de                  ; pad
        jp      _isr_cmd_loop

_isr_write_b1:
        ld      a, (de)
        ld      c, #OPL3_ADDR1      ; B don't-care for C4-C7
        out     (c), a
        inc     de
        ld      a, (de)
        inc     c                   ; C7 = data port
        out     (c), a
        inc     de
        inc     de
        jp      _isr_cmd_loop

_isr_do_wait:
        ld      a, (de)             ; lo
        inc     de
        ld      l, a
        ld      a, (de)             ; hi
        inc     de
        ld      h, a
        inc     de                  ; pad
        ld      (_isr_wait_ctr), hl
        ex      de, hl
        ld      (_isr_read_ptr), hl
        jp      _isr_exit

;--- AY8910 chip 1 write: [reg, val, pad] --------------------------------
_isr_write_ay:
        ld      bc, #0xFFFD
        ld      a, #0xFF            ; select first AY (TurboSound)
        out     (c), a
        ld      a, (de)             ; reg
        out     (c), a              ; select AY register
        inc     de
        ld      a, (de)             ; val
        ld      b, #0xBF            ; BC = #BFFD
        out     (c), a              ; write data
        inc     de
        inc     de                  ; pad
        jp      _isr_cmd_loop

;--- AY8910 chip 2 write (TurboSound): [reg, val, pad] -------------------
_isr_write_ay2:
        ld      bc, #0xFFFD
        ld      a, #0xFE            ; select second AY
        out     (c), a
        ld      a, (de)             ; reg
        out     (c), a              ; select register
        inc     de
        ld      a, (de)             ; val
        ld      b, #0xBF            ; BC = #BFFD
        out     (c), a
        inc     de
        inc     de                  ; pad
        ; Switch back to chip 1
        ld      bc, #0xFFFD
        ld      a, #0xFF
        out     (c), a
        jp      _isr_cmd_loop

;--- SAA1099 chip 1 write: [reg, val, pad] --------------------------------
_isr_write_saa:
        ld      a, (de)             ; reg
        ld      bc, #0x00FF         ; SAA1 address port
        out     (c), a
        inc     de
        ld      a, (de)             ; val
        ld      b, #0x01            ; BC = #01FF  SAA1 data port
        out     (c), a
        inc     de
        inc     de                  ; pad
        jp      _isr_cmd_loop

;--- SAA1099 chip 2 write: [reg, val, pad] --------------------------------
_isr_write_saa2:
        ld      a, (de)             ; reg
        ld      bc, #0x02FF         ; SAA2 address port
        out     (c), a
        inc     de
        ld      a, (de)             ; val
        ld      b, #0x03            ; BC = #03FF  SAA2 data port
        out     (c), a
        inc     de
        inc     de                  ; pad
        jp      _isr_cmd_loop

;--- CMD_INC_SEC: инкремент счётчика секунд [0,0,0] ---------------------
_isr_inc_sec:
        inc     de
        inc     de
        inc     de                  ; пропустить 3 байта параметров
        ld      hl, (_isr_play_seconds)
        inc     hl
        ld      (_isr_play_seconds), hl
        jp      _isr_cmd_loop

;--- CMD_SKIP_TICKS: skip N записей pos_table [N,0,0] ----------------------
; CMD_SKIP_TICKS(N):
;   N=0 — не перепрограммирует, просто exit (1 тик пауза)
;   N>=1 — пропустить N записей, перепрограммировать INT (N+1 тиков пауза)
;   Max N=55 (кадр минус 1).
_isr_skip_ticks:
        ld      a, (de)             ; N = skip count (0..55)
        inc     de
        inc     de
        inc     de                  ; пропустить 3 байта параметров
        ; Сохранить read_ptr
        ex      de, hl
        ld      (_isr_read_ptr), hl
        ; N=0: просто exit (как CMD_WAIT(0) без накладных на wait_ctr)
        or      a
        jp      z, _isr_exit
        ; Пропустить N записей pos_table: перейти по N-1 ссылкам,
        ; затем OUTI×3 из N-ой записи (перепрограммировать INT)
        ld      hl, (_pos_ptr)
        dec     a                   ; N-1 линков пропустить
        jr      z, _skip_program    ; N=1: сразу программируем текущую
        ld      b, a
_skip_loop:
        inc     hl                  ; пропустить VSINTH
        inc     hl                  ; VSINTL
        inc     hl                  ; HSINT
        ld      a, (hl)             ; next_lo
        inc     hl
        ld      h, (hl)             ; next_hi
        ld      l, a
        djnz    _skip_loop
_skip_program:
        ; HL = запись для перепрограммирования INT портов
        ld      bc, #0x25AF
        outi                        ; VSINTH (#24AF)
        outi                        ; VSINTL (#23AF)
        outi                        ; HSINT  (#22AF)
        ; Следующий next_ptr
        ld      a, (hl)
        inc     hl
        ld      h, (hl)
        ld      l, a
        ld      (_pos_ptr), hl
        jp      _isr_exit

;--- CMD_ISR_DONE: заморозить ISR, выставить флаг [0,0,0] ------------------
; ISR зацикливается на этой команде, не переключает буферы.
; Main loop ждёт isr_done == 1 перед завершением.
_isr_cmd_done:
        inc     de
        inc     de
        inc     de                  ; пропустить 3 байта параметров
        ; Выставить флаг готовности к завершению
        ld      a, #1
        ld      (_isr_done), a
        ; Перемотать read_ptr на начало CMD_ISR_DONE (DE - 4)
        ex      de, hl
        dec     hl
        dec     hl
        dec     hl
        dec     hl
        ld      (_isr_read_ptr), hl
        jp      _isr_exit

;--- CMD_CALL_WC: вызов оригинального WC handler [0,0,0] ------------------
; Внутренняя команда ISR (не используется buffer filler, зарезервировано)
_isr_call_wc:
        inc     de
        inc     de
        inc     de                  ; пропустить 3 байта параметров
        jp      _isr_cmd_loop       ; просто пропустить

;==============================================================================
; call_wc_handler() — Вызов WC ISR из main loop (C-callable)
;
; WC ожидает периодический вызов своего ISR для обновления TMN,
; клавиатуры, часов и др. Вызываем из main loop ~50 Гц.
;
; Механизм: DI → JP (saved_vec).  WC handler делает EI+RET
; обратно к вызывающему коду (tail-call через JP).
;==============================================================================
_call_wc_handler::
        ld      hl, (_isr_saved_vec)
        ld      a, h
        or      l
        ret     z               ; saved_vec==0 → isr_init не вызывался
        di
        jp      (hl)            ; tail-call: WC handler делает EI+RET

;==============================================================================
        .area _DATA

_isr_active_buf::   .db 0
_isr_enabled::      .db 0
_isr_tick_ctr::     .db 0          ; инкрементируется каждые ISR тик (2734/сек, wrap 256)
_isr_border_color:: .db 0          ; цвет бордюра при выходе из ISR (пишет main, читает ISR)
_isr_play_seconds:: .dw 0          ; инкрементируется CMD_INC_SEC
_isr_read_ptr::     .dw 0
_isr_wait_ctr::     .dw 0
_isr_done::         .db 0          ; 1 = ISR замер на CMD_ISR_DONE, готов к завершению
_pos_ptr:           .dw 0          ; инициализируется в isr_init()
_isr_saved_vec:     .dw 0          ; сохранённый вектор WC (#5BFF)

;==============================================================================
; Командные буферы (512 байт каждый)
;==============================================================================

        .even
_cmd_buf_a::
        .ds 512
_cmd_buf_b::
        .ds 512

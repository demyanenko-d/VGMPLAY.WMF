; ═══════════════════════════════════════════════════════════════════════════
; Модуль ISR - 2734 Гц, обработчик прерывания
; ═══════════════════════════════════════════════════════════════════════════
;
; Архитектура: ВСЕ значения для регистров INT предрассчитаны в таблице.
; ISR читает 3 байта через 3×OUTI — НИКАКИХ вычислений!
;
; Частота: 3,500,000 / 1280 = 2734.375 Hz (~44100/16, разница ~0.8%)
; Цикл: 56 тиков × 1280 = 71680 позиций = 1 видеокадр → ИДЕАЛЬНОЕ замыкание
; Таблица: 56 × 5 = 280 байт
;
; При 14 МГц с wait-state (~x2.5): ~3200 эффективных тактов между тиками.
;
; Коды команд:
;   CMD_WRITE_B0 = #40  [#40, reg, val, pad]  → OUT OPL3_B0_ADDR/DATA
;   CMD_WRITE_B1 = #80  [#80, reg, val, pad]  → OUT OPL3_B1_ADDR/DATA
;   CMD_WAIT     = #C0  [#C0, lo, hi,  pad]   → ISR_WAIT_CTR = HL
;   CMD_END_BUF  = #E0  [#E0, pad,pad, pad]   → переключить буфер
; ═══════════════════════════════════════════════════════════════════════════

; Коды команд (= смещение от ISR_HANDLER, определены в vgmplayer.asm)
; CMD_WRITE_B0 = #40, CMD_WRITE_B1 = #80, CMD_WAIT = #C0, CMD_END_BUF = #E0

; ═══════════════════════════════════════════════════════════════════════════
; ISR_INIT - Инициализация таблицы векторов IM2 и указателя позиции
; ═══════════════════════════════════════════════════════════════════════════
ISR_INIT:
        DI

        ; --- Заполнить таблицу векторов IM2 (257 байт, выровнено по 256) ---
        LD HL,IM2_TABLE
        LD DE,IM2_TABLE+1
        LD A,LOW ISR_HANDLER
        LD (HL),A
        LD BC,256
        LDIR
        LD HL,IM2_TABLE+#100
        LD (HL),HIGH ISR_HANDLER

        ; Установить регистр I
        LD A,HIGH IM2_TABLE
        LD I,A

        ; --- Разрешить только прерывание FRAME ---
        LD BC,PORT_INTMASK
        LD A,%00000001          ; только FRAME
        OUT (C),A

        ; --- Инициализировать указатель позиции ---
        LD HL,POS_TABLE
        LD (POS_PTR),HL         ; храним в переменной

        ; --- Инициализировать счётчик ожидания ---
        LD HL,0
        LD (ISR_WAIT_CTR),HL

        ; --- Записать начальную позицию в железо: строка 0, hpos 0 ---
        LD BC,PORT_HSINT
        XOR A
        OUT (C),A               ; HSINT = 0
        LD B,HIGH PORT_VSINTL
        OUT (C),A               ; VSINTL = 0
        LD B,HIGH PORT_VSINTH
        OUT (C),A               ; VSINTH = 0

        ; Переключиться в IM2
        IM 2
        RET

; ═══════════════════════════════════════════════════════════════════════════
; ISR_HANDLER - прерывание FRAME (2734 Гц)
; ─── OUTI-блок и таблицу не трогать! ────────────────────────────────────
; Команды: CMD_WRITE_B0=#40, CMD_WRITE_B1=#80, CMD_WAIT=#C0, CMD_END_BUF=#E0
; ═══════════════════════════════════════════════════════════════════════════
ISR_HANDLER:
        PUSH AF
        PUSH HL
        PUSH BC
        PUSH DE

        ; ─── OUTI×3: вывести позицию INT из таблицы (не трогать!) ───────
        LD HL,(POS_PTR)
        LD BC,#25AF             ; B=#25, C=#AF
        OUTI                    ; B→#24, OUT #24AF,(HL++) → VSINTH
        OUTI                    ; B→#23, OUT #23AF,(HL++) → VSINTL
        OUTI                    ; B→#22, OUT #22AF,(HL++) → HSINT
        LD A,(HL)               ; next_lo
        INC HL
        LD H,(HL)               ; next_hi
        LD L,A                  ; HL = next_ptr
        LD (POS_PTR),HL

        ; ─── SECOND_TIMER: счётчик секунд (ВСЕГДА, независимо от WAIT_CTR) ──
        LD HL,(SECOND_TIMER)
        DEC HL
        LD A,H
        OR L
        LD (SECOND_TIMER),HL
        JR NZ,ISR_NO_SEC
        ; Секунда истекла
        LD HL,ISR_FREQ
        LD (SECOND_TIMER),HL
        LD HL,(PLAY_SECONDS)
        INC HL
        LD (PLAY_SECONDS),HL
ISR_NO_SEC:

        ; ─── WAIT_CTR: если 0 — идти выполнять команды ──────────────────
        LD HL,(ISR_WAIT_CTR)
        LD A,H
        OR L
        JP Z,ISR_EXEC_CMD
        DEC HL
        LD (ISR_WAIT_CTR),HL

ISR_EXIT:
        XOR A
        OUT (#FE),A             ; DEBUG: чёрный = выход из ISR
        POP DE
        POP BC
        POP HL
        POP AF
        EI
        RET

; ─── Exec-путь ───────────────────────────────────────────────────────────
ISR_EXEC_CMD:
        LD A,(ISR_ENABLED)
        OR A
        JP Z,ISR_EXIT
        LD HL,(ISR_READ_PTR)
        EX DE,HL                ; DE = указатель потока команд

ISR_CMD_LOOP:
        LD A,(DE)
        INC DE
        CP CMD_WRITE_B0         ; #40
        JP Z,ISR_WRITE_B0
        CP CMD_WRITE_B1         ; #80
        JP Z,ISR_WRITE_B1
        CP CMD_WAIT             ; #C0
        JP Z,ISR_DO_WAIT
        ; CMD_END_BUF (#E0) — любое другое значение тоже сюда
ISR_DO_END_BUF:
        LD A,2
        OUT (#FE),A             ; DEBUG: красный = смена буфера
        LD A,(ISR_ACTIVE_BUF)
        XOR 1
        LD (ISR_ACTIVE_BUF),A
        JR NZ,ISR_END_USE_B
        LD DE,CMD_BUF_A
        JP ISR_CMD_LOOP         ; продолжать в новом буфере!
ISR_END_USE_B:
        LD DE,CMD_BUF_B
        JP ISR_CMD_LOOP         ; продолжать в новом буфере!

ISR_WRITE_B0:
        LD A,6
        OUT (#FE),A             ; DEBUG: жёлтый = запись OPL3
        LD A,(DE)               ; reg
        LD BC,OPL3_ADDR0        ; B=0, C=#C4 → полный адрес порта #00C4
        OUT (C),A
        INC DE
        LD A,(DE)               ; val
        LD BC,OPL3_DATA0        ; B=0, C=#C5 → полный адрес порта #00C5
        OUT (C),A
        INC DE
        INC DE                  ; pad
        JP ISR_CMD_LOOP

ISR_WRITE_B1:
        LD A,6
        OUT (#FE),A             ; DEBUG: жёлтый = запись OPL3
        LD A,(DE)               ; reg
        LD BC,OPL3_ADDR1        ; B=0, C=#C6 → полный адрес порта #00C6
        OUT (C),A
        INC DE
        LD A,(DE)               ; val
        LD BC,OPL3_DATA1        ; B=0, C=#C7 → полный адрес порта #00C7
        OUT (C),A
        INC DE
        INC DE                  ; pad
        JP ISR_CMD_LOOP

ISR_DO_WAIT:
        LD A,(DE)               ; wait_lo
        INC DE
        LD L,A
        LD A,(DE)               ; wait_hi
        INC DE
        LD H,A                  ; HL = wait counter
        INC DE                  ; pad
        LD (ISR_WAIT_CTR),HL
        EX DE,HL
        LD (ISR_READ_PTR),HL    ; сохранить позицию потока
        JP ISR_EXIT

; ─── Переменные ISR ──────────────────────────────────────────────────────
POS_PTR:        DW POS_TABLE    ; указатель на текущую запись в таблице
SECOND_TIMER:   DW ISR_FREQ     ; обратный счётчик 2734 → 0 = 1 секунда


; ═══════════════════════════════════════════════════════════════════════════
; Таблица векторов IM2 (выравнивание по 256 байт, размер 257 байт)
; ═══════════════════════════════════════════════════════════════════════════
        ALIGN 256
IM2_TABLE:
        DS 257

; ═══════════════════════════════════════════════════════════════════════════
; Командные буферы (двойная буферизация)
; ═══════════════════════════════════════════════════════════════════════════
        ALIGN 4
CMD_BUF_A:
        DS CMD_BUF_SIZE
CMD_BUF_B:
        DS CMD_BUF_SIZE

; ═══════════════════════════════════════════════════════════════════════════
; Таблица позиций - 2734 Гц (шаг=1280, 56 записей, чистый цикл)
; Формат: [VSINTH, VSINTL, HSINT] для последовательности 3×OUTI
; Размер: 280 байт
; ═══════════════════════════════════════════════════════════════════════════
        INCLUDE "src/pos_table_43750.inc"

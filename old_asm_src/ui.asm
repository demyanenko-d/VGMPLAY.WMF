; ═══════════════════════════════════════════════════════════════════════════
; Модуль UI - текстовый интерфейс плеера
; ═══════════════════════════════════════════════════════════════════════════
; Использует WC API для рисования окна.
;
; Обязательный порядок при старте плагина:
;   1. CALL GEDPL         ; восстановить дисплей/палитру WC (обязательно!)
;   2. CALL UI_INIT       ; нарисовать окно (IX = PLWND)
;   3. CALL UI_DRAW_INFO  ; заполнить содержимое
;
; Все оконные функции (PRWOW, RRESB, TXTPR) требуют IX = SOW (PLWND)
; ═══════════════════════════════════════════════════════════════════════════

; ─── Структура окна SOW (Structure Of Window) ─────────────────────────────
; Должна лежать в #8000-#BFFF или #0000-#3FFF.
; +8 (DW) = 0 при первом вызове PRWOW, WC заполнит адресом буфера.
PLWND:
        DB 0                    ; +0: тип 0 = стандартное окно с рамкой
        DB 0                    ; +1: маска цвета курсора
        DB WND_X                ; +2: X позиция на экране
        DB WND_Y                ; +3: Y позиция на экране
        DB WND_W                ; +4: ширина окна
        DB WND_H                ; +5: высота окна
        DB CLR_MAIN             ; +6: цвет (paper+ink)
        DB 0                    ; +7: резерв (всегда 0!)
        DW 0                    ; +8,+9: буфер (0 = не выведено, WC заполнит)
        DB 0                    ; +10: разделитель 1 (0 = нет)
        DB 0                    ; +11: разделитель 2 (0 = нет)

; ═══════════════════════════════════════════════════════════════════════════
; UI_INIT - Вывести окно плеера на экран
; ═══════════════════════════════════════════════════════════════════════════
UI_INIT:
        PUSH AF
        PUSH BC
        PUSH DE
        PUSH HL

        LD IX,PLWND             ; IX = SOW (обязательно для PRWOW!)
        CALL PRWOW              ; нарисовать окно

        POP HL
        POP DE
        POP BC
        POP AF
        RET

; ═══════════════════════════════════════════════════════════════════════════
; UI_DRAW_INFO - Нарисовать информацию о файле в окне
; ═══════════════════════════════════════════════════════════════════════════
UI_DRAW_INFO:
        PUSH AF
        PUSH BC
        PUSH DE
        PUSH HL

        ; Заголовок плеера
        LD IX,PLWND             ; IX = SOW
        LD D,1                  ; Y = 1 (внутри окна)
        LD E,2                  ; X = 2 (внутри окна)
        LD HL,STR_TITLE
        CALL TXTPR

        ; Строка подсказки клавиш
        LD IX,PLWND
        LD D,2
        LD E,2
        LD HL,STR_KEYS
        CALL TXTPR

        ; Статус (Playing/Paused)
        CALL UI_DRAW_STATUS

        POP HL
        POP DE
        POP BC
        POP AF
        RET

; ═══════════════════════════════════════════════════════════════════════════
; UI_DRAW_STATUS - Показать статус воспроизведения
; ═══════════════════════════════════════════════════════════════════════════
UI_DRAW_STATUS:
        PUSH AF
        PUSH BC
        PUSH DE
        PUSH HL

        LD A,(PAUSED)
        OR A
        JR NZ,UI_SHOW_PAUSED
        LD HL,STR_PLAYING
        JR UI_SHOW_STATUS
UI_SHOW_PAUSED:
        LD HL,STR_PAUSED
UI_SHOW_STATUS:
        LD IX,PLWND             ; IX = SOW
        LD D,4                  ; Y = 4 (внутри окна)
        LD E,2
        CALL TXTPR

        POP HL
        POP DE
        POP BC
        POP AF
        RET

; ═══════════════════════════════════════════════════════════════════════════
; UI_UPDATE - Периодическое обновление (вызывается из главного цикла)
; ═══════════════════════════════════════════════════════════════════════════
UI_UPDATE:
        PUSH AF
        PUSH BC
        PUSH DE
        PUSH HL

        ; --- Вычислить MM:SS из PLAY_SECONDS ---
        LD HL,(PLAY_SECONDS)
        LD B,0                  ; B = minutes
UI_DIV60:
        LD A,H
        OR L
        JR Z,UI_DIV_DONE        ; HL=0 → B=min, HL=0=sec
        LD DE,-60               ; 0xFFC4
        ADD HL,DE               ; HL -= 60; carry set ← HL_orig >= 60
        JR NC,UI_DIV_UNDO       ; нет carry → HL_orig < 60 → восстановить
        INC B                   ; засчитать минуту
        JR UI_DIV60
UI_DIV_UNDO:
        LD DE,60
        ADD HL,DE               ; восстановить HL = остаток (секунды)
UI_DIV_DONE:
        LD C,L                  ; C = секунды (0-59)

        ; --- Записать MM в TIME_BUF+6,7 ---
        LD A,B
        LD HL,TIME_BUF+6
        CALL UI_BYTE2DEC

        ; --- Записать SS в TIME_BUF+9,10 ---
        LD A,C
        LD HL,TIME_BUF+9
        CALL UI_BYTE2DEC

        ; --- Отобразить ---
        LD IX,PLWND
        LD D,6
        LD E,2
        LD HL,TIME_BUF
        CALL TXTPR

        POP HL
        POP DE
        POP BC
        POP AF
        RET

; UI_BYTE2DEC: конвертировать A (0-99) в 2 ASCII-цифры по адресу (HL)
; Меняет: A, F
UI_BYTE2DEC:
        LD B,0
UI_B2D_L:
        CP 10
        JR C,UI_B2D_END
        SUB 10
        INC B
        JR UI_B2D_L
UI_B2D_END:
        ; B=десятки, A=единицы
        PUSH AF
        LD A,B
        ADD A,'0'
        LD (HL),A
        INC HL
        POP AF
        ADD A,'0'
        LD (HL),A
        RET

; ═══════════════════════════════════════════════════════════════════════════
; UI_REMOVE - Закрыть окно плеера (восстановить экран)
; ═══════════════════════════════════════════════════════════════════════════
UI_REMOVE:
        PUSH AF
        PUSH BC
        PUSH DE
        PUSH HL

        LD IX,PLWND             ; IX = SOW (обязательно для RRESB!)
        CALL RRESB              ; восстановить экран под окном

        POP HL
        POP DE
        POP BC
        POP AF
        RET

; ═══════════════════════════════════════════════════════════════════════════
; SHOW_ERROR / SHOW_WARNING - Заглушка (не реализовано)
; ═══════════════════════════════════════════════════════════════════════════
SHOW_ERROR:
SHOW_WARNING:
        RET

; ═══════════════════════════════════════════════════════════════════════════
; Строки UI (завершаются нулём — формат WC TXTPR)
; ═══════════════════════════════════════════════════════════════════════════
STR_TITLE:      DB "VGM Player v2.0 (OPL3)",0
STR_KEYS:       DB "ESC-Exit  SPC-Next  P-Prev  M-Pause",0
STR_PLAYING:    DB "Playing...          ",0
STR_PAUSED:     DB "** PAUSED **        ",0
; TIME_BUF: динамический буфер для отображения времени (изменяется в UI_UPDATE)
TIME_BUF:       DB "Time: 00:00",0

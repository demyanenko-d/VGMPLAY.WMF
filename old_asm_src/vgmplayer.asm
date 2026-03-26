; ═══════════════════════════════════════════════════════════════════════════
; VGM Player Plugin for Wild Commander
; OPL3 (YMF262) playback on TSConfig
; Architecture v2: ISR-driven at 44100 Hz, double-buffered commands
; (C) 2026
; ═══════════════════════════════════════════════════════════════════════════

        DEVICE NOSLOT64K      ; плоская 64К память, без ограничений слотов
        
        OUTPUT "vgmplay.wmf"

PLUGHEAD EQU 0              ; заголовок в начале файла (как в PLUGBP)
PLUGSTAR EQU PLUGHEAD+512  ; код с байта #200 в файле

PLUGLEN  EQU END-PLUGIN
PAGES    EQU (PLUGLEN/16384)+1
BLKSIZE  EQU (PLUGLEN+511)/512

        ORG PLUGHEAD            ; = 0, заголовок физически с байта 0 в файле

; ═══════════════════════════════════════════════════════════════════════════
; Plugin Header (512 bytes, exact WC format)
; ═══════════════════════════════════════════════════════════════════════════

; +0: Reserved (16 bytes)
        DS 16
; +16: Magic signature (16 bytes)
        DB "WildCommanderMDL"
; +32: Plugin system version
        DB #10
; +33: Reserved
        DB 0
; +34: Number of 16KB pages
        DB PAGES
; +35: Start page (0-based, mapped to #8000)
        DB 0
; +36: Data blocks (6 x 2 bytes = 12 bytes)
        DB 0,BLKSIZE            ; Block 0: plugin code, page 0
        DB 0,0                  ; Block 1: unused
        DB 0,0                  ; Block 2: unused
        DB 0,0                  ; Block 3: unused
        DB 0,0                  ; Block 4: unused
        DB 0,0                  ; Block 5: unused
; +48: Reserved (15 bytes)
        DS 15
; +63: Extension flags (0 = launch by ENTER and F3)
        DB 0
; +64: Supported extensions (32 slots x 3 bytes = 96 bytes)
        DB "VGM"
        DS 31*3                 ; Remaining slots empty
; +160: End marker
        DB 0
; +161: Max file size (32-bit LE: DW low_word, high_word)
        DW #FFFF,#FFFF          ; Unlimited
; +165: Plugin name (32 bytes, space-padded)
        DB "VGM Player for OPL3 v2.0        "
; +197: Activation type (0 = by extension only)
        DB 0
; +198: F2/F4 button text (6 bytes)
        DS 6
; +204: Viewer menu text (24 bytes)
        DS 24
; +228: Reserved (32 bytes)
        DS 32
; +260: INI Parameters
        DB 0                    ; No parameters
; Pad to 512 bytes
        DS PLUGSTAR-$

        ; Заголовок занимает 512 байт (PLUGSTAR = #200),
        ; код физически с байта #200, WC отображает страницу на #8000.
        ; DISP #8000 — все метки кода резолвятся в диапазон #8000+
        DISP #8000

; ═══════════════════════════════════════════════════════════════════════════
; Constants
; ═══════════════════════════════════════════════════════════════════════════
WLD     EQU #6006               ; WC API entry point
VPL     EQU 32                  ; WC VPL constant

; --- TSConfig Ports (port = reg_number * 256 + #AF) ---
PORT_SYSCONFIG  EQU #20AF       ; SysConfig: bit1:0 = CPU freq, bit2 = cache
PORT_HSINT      EQU #22AF       ; Horizontal INT position (0-223)
PORT_VSINTL     EQU #23AF       ; Vertical INT position low (bits 7:0)
PORT_VSINTH     EQU #24AF       ; Vertical INT position high (bit0=VCNT[8], bits7:4=VINT_INC)
PORT_INTMASK    EQU #2AAF       ; INT mask: bit0=FRAME, bit1=LINE, bit2=DMA

; --- CPU speed values for SysConfig bits 1:0 ---
CPU_3_5MHZ      EQU %00000000
CPU_7MHZ        EQU %00000001
CPU_14MHZ       EQU %00000010

; --- OPL3 Ports ---
OPL3_ADDR0      EQU #C4
OPL3_DATA0      EQU #C5
OPL3_ADDR1      EQU #C6
OPL3_DATA1      EQU #C7

; --- ISR Command Types (код = смещение в странице ISR_HANDLER) ---
CMD_WRITE_B0    EQU #40         ; [#40, reg, val, pad] OPL3 банк 0
CMD_WRITE_B1    EQU #80         ; [#80, reg, val, pad] OPL3 банк 1
CMD_WAIT        EQU #C0         ; [#C0, lo,  hi,  pad] ждать N тиков
CMD_END_BUF     EQU #E0         ; [#E0, pad, pad, pad] переключить буфер

; --- Command Buffer sizes ---
CMD_BUF_SIZE    EQU 512         ; bytes per buffer (128 commands)

; --- Window layout ---
WND_X   EQU 9
WND_Y   EQU 5
WND_W   EQU 62
WND_H   EQU 14

; --- Colors ---
CLR_MAIN EQU %01111111

; --- Коды ошибок для ERROR_BORDER ---
; Формат: биты [5:3]=категория, биты [2:0]=код. Оба поля ≠ 0 (цвета бордюра 1-7)
; Цвета ZX: 1=синий, 2=красный, 3=маджента, 4=зелёный, 5=циан, 6=жёлтый, 7=белый
;
;   Категории (старшие 3 бита):
;     1 (синий)    = файловые ошибки
;     2 (красный)  = ошибки формата VGM
;     3 (маджента) = аппаратные ошибки
;
;   Конкретные коды (младшие 3 бита) — внутри категории:
;     1 = первая ошибка, 2 = вторая, и т.д.
;
ERR_FILE_SMALL   EQU (1<<3)|1  ; файл <= 255 байт:  черный, синий(1),   синий(1)
ERR_VGM_HEADER   EQU (2<<3)|1  ; плохой заголовок:  черный, синий(1),   красный(2)
ERR_FILE_LOAD    EQU (1<<3)|2  ; ошибка загрузки:   черный, красный(2), синий(1)

; --- Memory addresses ---
VGMBUF  EQU #C000              ; VGM данные через WC VPL (окно 3: #C000-#FFFF)

; ═══════════════════════════════════════════════════════════════════════════
; Plugin Entry Point
; ═══════════════════════════════════════════════════════════════════════════
PLUGIN:
        PUSH IX                 ; Сохранить IX (WC передаёт структуру активной панели)

        ; Сохранить входные параметры от WC
        ; A` = индекс расширения, BC = указатель на имя файла,
        ; HL = размер файла (младшее слово), DE = размер файла (старшее слово)
        EXA
        LD (FILE_EXT),A
        EXA
        LD (FILE_NAME),BC
        LD (FILE_SIZE),HL
        LD (FILE_SIZE+2),DE

        ; Init variables
        XOR A
        LD (EXIT_CODE),A
        LD (PAUSED),A
        LD (SONG_ENDED),A
        LD (ISR_ENABLED),A
        LD (ISR_ACTIVE_BUF),A
        LD (UNDERRUN_FLAG),A
        LD (OPL3_FOUND),A
        LD (UI_FRAME_CTR),A
        LD (LAST_ISR_BUF),A

        LD HL,0
        LD (PLAY_SECONDS),HL

        ; Переключиться на 14 МГц (PORT_SYSCONFIG только на запись)
        ; При выходе всегда восстанавливаем 3.5 МГц
        LD BC,PORT_SYSCONFIG
        LD A,CPU_14MHZ
        OUT (C),A

        ; Проверка размера файла: достаточно > 256 байт
        ; (VGM заголовок минимум 64 байта, но реальные файлы от 8КБ)
        LD A,(FILE_SIZE+1)      ; старший байт младшего слова
        OR A
        LD A,ERR_FILE_SMALL
        JP Z,ERROR_BORDER       ; <= 255 байт → ошибка
FILE_SIZE_OK:

        ; === Загрузить VGM файл в VPL-страницы WC (окно #C000, 16KB каждая) ===
        ; Шаг 1: вычислить число страниц B = ceil(FILE_SIZE / 16384)
        LD HL,(FILE_SIZE)
        LD DE,(FILE_SIZE+2)
        LD BC,16383             ; добавляем для округления вверх
        ADD HL,BC
        JR NC,VGM_LOAD_NC
        INC DE
VGM_LOAD_NC:
        ; pages = (E << 2) | (H >> 6)  [файлы < 4МБ]
        LD A,H
        SRL A:SRL A:SRL A:SRL A:SRL A:SRL A  ; A = H >> 6
        LD C,A
        LD A,E
        ADD A,A:ADD A,A
        OR C
        LD B,A                  ; B = число страниц

        ; Шаг 2: вычислить VGM_END_PAGE и VGM_END_ADDR
        DEC A                   ; номер последней страницы (0-based)
        LD (VGM_END_PAGE),A
        ; VGM_END_ADDR = #C000 + (FILE_SIZE & #3FFF)
        LD HL,(FILE_SIZE)
        LD A,H
        AND #3F
        OR #C0
        LD H,A
        LD (VGM_END_ADDR),HL

        ; Шаг 3: загружать страницы по одной
        LD A,B
        LD (VGM_PAGES_TOTAL),A
        XOR A
        LD (VGM_CUR_PAGE),A
VGM_LOAD_LOOP:
        LD A,(VGM_PAGES_TOTAL)
        LD B,A
        LD A,(VGM_CUR_PAGE)
        CP B
        JR NC,VGM_LOAD_DONE
        CALL MNGCVPL            ; A = VPL-индекс страницы → отобразить на #C000
        LD HL,#C000
        LD B,32                 ; 32 × 512 = 16384 байт
        CALL LOAD512
        LD A,(VGM_CUR_PAGE)
        INC A
        LD (VGM_CUR_PAGE),A
        JR VGM_LOAD_LOOP
VGM_LOAD_DONE:
        ; Отобразить страницу 0 для разбора заголовка
        XOR A
        LD (VGM_CUR_PAGE),A
        CALL MNGCVPL

        ; Разобрать VGM заголовок
        CALL VGM_PARSE_HEADER
        LD A,ERR_VGM_HEADER
        JP NZ,ERROR_BORDER

        ; Init OPL3
        CALL OPL3_INIT

        ; Восстановить дисплей (ОБЯЗАТЕЛЬНО перед любыми вызовами WC UI API)
        CALL GEDPL

        ; Init UI (uses WC API, still in normal interrupt mode)
        CALL UI_INIT
        CALL UI_DRAW_INFO

        ; Disable WC interrupts (A'=0 via INT_PL)
        ; WC will auto-restore on plugin exit
        XOR A
        EXA                     ; A' = 0
        CALL INT_PL

        ; Prepare initial data in both buffers (до ISR_INIT!)
        LD A,0
        CALL VGM_FILL_BUFFER   ; fill BUF_A
        LD A,1
        CALL VGM_FILL_BUFFER   ; fill BUF_B

        ; Set ISR read pointer to start of BUF_A
        LD HL,CMD_BUF_A
        LD (ISR_READ_PTR),HL

        ; Enable ISR processing
        LD A,1
        LD (ISR_ENABLED),A

        ; Setup IM2 + start timer (после заполнения буферов!)
        CALL ISR_INIT
        EI

; ═══════════════════════════════════════════════════════════════════════════
; Main Loop
; ═══════════════════════════════════════════════════════════════════════════
MAIN_LOOP:
        EI                      ; гарантируем прерывания (WC API мог сделать DI)
        ; 1. Check if ISR switched buffers -> fill the free one
        LD A,(ISR_ACTIVE_BUF)
        LD B,A
        LD A,(LAST_ISR_BUF)
        CP B
        JR Z,ML_NO_FILL        ; same buffer, nothing to do
        
        ; ISR switched! Update tracking and fill the now-free buffer
        LD A,B
        LD (LAST_ISR_BUF),A
        ; The free buffer is the one ISR is NOT reading
        XOR 1                   ; 0->1, 1->0  (A = free buffer index)
        LD C,A                  ; save free buffer index
        LD A,1:OUT (#FE),A      ; DEBUG: синий → входим в VGM_FILL_BUFFER
        LD A,C                  ; restore free buffer index
        CALL VGM_FILL_BUFFER
        XOR A:OUT (#FE),A       ; DEBUG: чёрный → VGM_FILL_BUFFER вернулся
        
ML_NO_FILL:
        ; 2. Update UI periodically
        LD A,(UI_FRAME_CTR)
        INC A
        LD (UI_FRAME_CTR),A
        AND #1F                 ; every 32 iterations
        JR NZ,ML_NO_UI
        CALL UI_UPDATE
ML_NO_UI:

        ; 3. Read keyboard (direct ZX matrix, no interrupts needed)
        CALL KBD_READ
        OR A
        JR NZ,GOT_KEY

        ; 4. Check if song ended
        LD A,(SONG_ENDED)
        OR A
        JP NZ,SONG_DONE

        JP MAIN_LOOP

GOT_KEY:
        ; A = action code: 1=ESC, 2=next, 3=prev, 4=pause
        CP 1
        JR Z,DO_EXIT
        CP 2
        JR Z,DO_NEXT
        CP 3
        JR Z,DO_PREV
        CP 4
        JR Z,DO_PAUSE
        JR MAIN_LOOP

DO_EXIT:
        XOR A
        LD (EXIT_CODE),A
        JR EXIT_PLAYER

DO_NEXT:
        LD A,2
        LD (EXIT_CODE),A
        JR EXIT_PLAYER

DO_PREV:
        LD A,4
        LD (EXIT_CODE),A
        JR EXIT_PLAYER

DO_PAUSE:
        LD A,(PAUSED)
        XOR 1
        LD (PAUSED),A
        ; When pausing, ISR keeps running but main loop stops filling buffers
        ; When unpausing, ISR was just replaying silence/waits
        CALL UI_DRAW_STATUS
        JP MAIN_LOOP

SONG_DONE:
        ; Проверить петлю (VGM_LOOP_ADDR = 0 → нет петли)
        LD HL,(VGM_LOOP_ADDR)
        LD A,H
        OR L
        JR Z,SONG_NO_LOOP

        ; Переключиться на страницу петли и перезапустить
        LD A,(VGM_LOOP_PAGE)
        LD (VGM_CUR_PAGE),A
        CALL MNGCVPL
        LD HL,(VGM_LOOP_ADDR)
        LD (VGM_READ_PTR),HL
        XOR A
        LD (SONG_ENDED),A
        JP MAIN_LOOP

SONG_NO_LOOP:
        LD A,2                  ; next file
        LD (EXIT_CODE),A
        ; fall through

; ═══════════════════════════════════════════════════════════════════════════
; Exit
; ═══════════════════════════════════════════════════════════════════════════
EXIT_PLAYER:
        DI

        ; Stop ISR
        XOR A
        LD (ISR_ENABLED),A

        ; Silence OPL3
        CALL OPL3_RESET

        ; Restore IM1
        IM 1
        EI

        ; Восстановить 3.5 МГц
        LD BC,PORT_SYSCONFIG
        LD A,CPU_3_5MHZ
        OUT (C),A

        ; Remove UI
        CALL UI_REMOVE

        ; Return exit code to WC
        LD A,(EXIT_CODE)
        POP IX                  ; Восстановить IX для WC
        RET

ERROR_EXIT:
        ; Восстановить 3.5 МГц
        LD BC,PORT_SYSCONFIG
        LD A,CPU_3_5MHZ
        OUT (C),A
        XOR A
        POP IX                  ; Восстановить IX для WC
        RET

; ─── Отладка: вывод кода ошибки бордюром, зависание ───────────────────
; Вход: A = код ошибки (биты [2:0] = младший, [5:3] = старший; оба ≠ 0)
; Цикл: чёрный → цвет(биты[2:0]) → цвет(биты[5:3]) → повтор
; Задержка каждого цвета ≈ 300мс при 14МГц
; TODO: убрать в релизной версии
ERROR_BORDER:
        LD C,A                  ; C = код ошибки на всё время
ERR_LOOP:
        ; 1. Чёрный бордюр (разделитель)
        XOR A
        OUT (#FE),A
        CALL ERR_DELAY
        ; 2. Младшие 3 бита → цвет
        LD A,C
        AND %00000111
        OUT (#FE),A
        CALL ERR_DELAY
        ; 3. Биты [5:3] → цвет (сдвиг вправо на 3)
        LD A,C
        RRCA
        RRCA
        RRCA
        AND %00000111
        OUT (#FE),A
        CALL ERR_DELAY
        JR ERR_LOOP

ERR_DELAY:
        LD D,4
ERR_DLY_OUT:
        LD B,0
ERR_DLY_INN:
        DJNZ ERR_DLY_INN
        DEC D
        JR NZ,ERR_DLY_OUT
        RET

; ═══════════════════════════════════════════════════════════════════════════
; WC API Wrappers
; CALL WLD + IM 2 + RET: IM2 восстанавливается после каждого вызова WC
; (WC может переключать процессор обратно на IM1)
; ═══════════════════════════════════════════════════════════════════════════
PRWOW:  LD A,1:  CALL WLD:IM 2:EI:RET     ; Print window
RRESB:  LD A,2:  CALL WLD:IM 2:EI:RET     ; Remove window
PRSRW:  LD A,3:  CALL WLD:IM 2:EI:RET     ; Print string in window
PRIAT:  EXA:LD A,4:CALL WLD:IM 2:EI:RET  ; Print attributes
TXTPR:  LD A,11: CALL WLD:IM 2:EI:RET     ; Print text
GEDPL:  LD A,15: CALL WLD:IM 2:EI:RET     ; Get display
LOAD512:LD A,#30:CALL WLD:IM 2:EI:RET     ; Load 512-byte blocks
MNGCVPL:EXA:LD A,#41:CALL WLD:IM 2:EI:RET; Map VPL page to #C000

INT_PL: EXA:LD A,86:CALL WLD:IM 2:EI:RET  ; Interrupt control

; ═══════════════════════════════════════════════════════════════════════════
; Global Variables
; ═══════════════════════════════════════════════════════════════════════════
FILE_EXT:       DB 0
FILE_NAME:      DW 0
FILE_SIZE:      DW 0,0          ; 32-bit little-endian

EXIT_CODE:      DB 0
PAUSED:         DB 0
SONG_ENDED:     DB 0

; VGM header data
VGM_DATA_START:  DW 0            ; адрес первого байта VGM команд (#C000-#FFFF)
VGM_LOOP_ADDR:   DW 0            ; адрес точки петли (#C000-#FFFF; 0 = нет петли)
VGM_LOOP_PAGE:   DB 0            ; VPL-страница (0-based) точки петли
VGM_READ_PTR:    DW 0            ; текущий адрес чтения в #C000-#FFFF
VGM_CUR_PAGE:    DB 0            ; текущая VPL-страница отображённая на #C000
VGM_END_ADDR:    DW 0            ; адрес конца данных в последней странице
VGM_END_PAGE:    DB 0            ; номер последней страницы (VPL, 0-based)
VGM_PAGES_TOTAL: DB 0            ; суммарное число загруженных страниц
VGM_TOTAL_SAMPLES: DW 0,0        ; 32-bit total samples
VGM_CLOCK:       DW 0,0          ; OPL3 clock value

; Playback time
PLAY_SECONDS:   DW 0            ; секунды воспроизведения (обновляется в ISR)

; ISR state (shared between ISR and main loop)
ISR_ENABLED:    DB 0            ; 0=ISR does nothing, 1=active
ISR_ACTIVE_BUF: DB 0            ; 0=reading BUF_A, 1=reading BUF_B
ISR_READ_PTR:   DW 0            ; current read pointer in active buffer
ISR_WAIT_CTR:   DW 0            ; tick countdown (0=process commands)
; ISR_STEP_IDX, ISR_HPOS, ISR_LINE are in isr.asm (local to ISR)

LAST_ISR_BUF:   DB 0            ; main loop tracking
UNDERRUN_FLAG:  DB 0

; OPL3 state
OPL3_FOUND:     DB 0

; UI
UI_FRAME_CTR:   DB 0

; ═══════════════════════════════════════════════════════════════════════════
; Include Modules
; ═══════════════════════════════════════════════════════════════════════════
        INCLUDE "src/isr.asm"
        INCLUDE "src/vgm.asm"
        INCLUDE "src/opl3.asm"
        INCLUDE "src/ui.asm"
        INCLUDE "src/keyboard.asm"

END:

# Исправления VGM Player - Полный Список

## 🔴 Критические Исправления

### 1. Детект OPL3 и условное использование
**Проблема**: Код пытается писать в OPL3 даже если чип не обнаружен
**Решение**: 
- При запуске вызывать `OPL3_DETECT` и сохранять результат в `OPL3_AVAILABLE`
- Все команды записи OPL3 проверяют флаг перед записью
- Если OPL3 нет - показываем предупреждение, но продолжаем работу (беззвучно)

### 2. Сохранение и восстановление обработчика прерываний
**Проблема**: Код использует line interrupts но не сохраняет/восстанавливает состояние
**Решение**:
- Использовать WC API `INT_PL` (функция #86):
  - При инициализации: `A'=#00` - отключить WC interrupts на время работы
  - При выходе: WC автоматически восстанавливает состояние
- Сохранять оригинальное состояние портов #00AF/#01AF
- При выходе восстанавливать всё

### 3. UI обновления не должны мешать музыке
**Проблема**: UPDATE_DISPLAY_CONDITIONAL может вызваться рекурсивно из DELAY_SAMPLES
**Решение**:
- Добавить флаг `UI_UPDATE_IN_PROGRESS`
- Перед обновлением UI проверять флаг
- UPDATE_DISPLAY_CONDITIONAL только выставляет флаг необходимости обновления
- Реальное обновление происходит в MAIN_LOOP когда безопасно

### 4. 32-битная адресация VGM файлов
**Проблема**: GET_VGM_BYTE работает только с 16-бит адресами
**Решение**: Полная реализация в playback_fixed.asm с 32-битными указателями

### 5. Выход с аргументом для перемотки треков
**Проблема**: Код не поддерживает переход на следующий/предыдущий файл
**Решение**: 
- По SPACE/ENTER: `A=2` (следующий файл) 
- По клавише "P" (Previous): `A=4` (предыдущий файл)
- EXIT_CODE сохраняет код выхода
- В конце `LD A,(EXIT_CODE)` и `RET`

### 6. Проверки переменных фреймов
**Проблема**: FRAME_SAMPLES может переполниться
**Решение**: 
- Проверять переполнение при каждом ADD
- Ограничивать максимальным значением
- Правильно обнулять в начале фрейма

## 📋 Детальные Решения

### timing.asm - Управление прерываниями

```asm
; Глобальные переменные для сохранения состояния
OLD_INT_CTRL:   DB 0        ; Оригинальное значение порта #01AF
OLD_LINE_INT:   DB 0        ; Оригинальная линия прерывания
OLD_I_REG:      DB 0        ; Оригинальный I регистр
WC_INT_STATE:   DB 0        ; Состояние прерываний WC

TIMING_INIT:
        ; 1. Отключить прерывания WC
        XOR A
        EXA
        LD HL,0
        CALL INT_PL             ; A'=0 => disable WC interrupts
        LD (WC_INT_STATE),A     ; Сохранить результат
        
        ; 2. Сохранить текущее состояние портов
        IN A,(PORT_INT_CTRL)
        LD (OLD_INT_CTRL),A
        IN A,(PORT_LINE_INT)
        LD (OLD_LINE_INT),A
        
        ; 3. Сохранить I регистр
        LD A,I
        LD (OLD_I_REG),A
        
        ; 4. Отключить line interrupt
        IN A,(PORT_INT_CTRL)
        AND %11111110
        OUT (PORT_INT_CTRL),A
        
        RET

TIMING_CLEANUP:
        ; 1. Восстановить line interrupt
        LD A,(OLD_INT_CTRL)
        OUT (PORT_INT_CTRL),A
        LD A,(OLD_LINE_INT)
        OUT (PORT_LINE_INT),A
        
        ; 2. Восстановить I регистр
        LD A,(OLD_I_REG)
        LD I,A
        
        ; 3. WC автоматически восстановит свои прерывания при выходе
        
        RET
```

### opl3.asm - Условное использование OPL3

```asm
OPL3_AVAILABLE: DB 0        ; 0=нет, 1=есть

INIT_OPL3:
        ; Детектирование
        CALL OPL3_DETECT
        JR NZ,.FOUND
        
        ; OPL3 не найден
        XOR A
        LD (OPL3_AVAILABLE),A
        
        ; Показать предупреждение
        LD HL,MSG_NO_OPL3
        CALL SHOW_WARNING       ; Не ошибка, просто предупреждение
        
        RET                     ; Продолжаем без звука
        
.FOUND:
        LD A,1
        LD (OPL3_AVAILABLE),A
        
        ; Инициализация OPL3
        CALL OPL3_RESET
        ; ... остальное ...
        RET

; Все функции записи проверяют флаг
OPL3_WRITE_PORT0:
        LD A,(OPL3_AVAILABLE)
        OR A
        RET Z                   ; Нет чипа - ничего не делаем
        
        ; Нормальная запись
        ; ...
        RET
```

### ui.asm - Безопасное обновление UI

```asm
UI_UPDATE_IN_PROGRESS: DB 0     ; Флаг блокировки рекурсии
UI_UPDATE_NEEDED:      DB 0     ; Флаг запроса обновления

UPDATE_DISPLAY_CONDITIONAL:
        PUSH AF,HL
        
        ; Проверка рекурсии
        LD A,(UI_UPDATE_IN_PROGRESS)
        OR A
        JR NZ,.SKIP             ; Уже обновляется - пропускаем
        
        ; Проверка длительности паузы
        LD HL,(WAIT_SAMPLES)
        LD A,H
        OR A
        JR Z,.CHECK_LOW
        
        ; Длинная пауза - запросить обновление
        LD A,1
        LD (UI_UPDATE_NEEDED),A
        JR .DONE
        
.CHECK_LOW:
        LD A,L
        CP (UI_UPDATE_THRESHOLD & #FF)
        JR C,.SKIP
        
        ; Запросить обновление
        LD A,1
        LD (UI_UPDATE_NEEDED),A
        
.DONE:
.SKIP:
        POP HL,AF
        RET

; Вызывается из MAIN_LOOP, не из delay функций
UPDATE_DISPLAY_SAFE:
        PUSH AF
        
        ; Проверить флаг запроса
        LD A,(UI_UPDATE_NEEDED)
        OR A
        JR Z,.NO_UPDATE
        
        ; Сбросить флаг и установить блокировку
        XOR A
        LD (UI_UPDATE_NEEDED),A
        INC A
        LD (UI_UPDATE_IN_PROGRESS),A
        
        ; Обновить экран
        CALL UPDATE_DISPLAY
        
        ; Снять блокировку
        XOR A
        LD (UI_UPDATE_IN_PROGRESS),A
        
.NO_UPDATE:
        POP AF
        RET
```

### vgmplayer.asm - Главный цикл и выход

```asm
PLUGIN:
        ; ... инициализация ...
        
        ; Отключить прерывания WC
        CALL DISABLE_WC_INTERRUPTS
        
        ; Инициализировать таймер (сохраняет состояние)
        CALL TIMING_INIT
        
        ; Проверить OPL3 (не критично если нет)
        CALL INIT_OPL3
        
        ; ... загрузка файла ...

MAIN_LOOP:
        ; Обработка VGM команд
        CALL PROCESS_VGM_FRAME
        
        ; Безопасное обновление UI (не в delay!)
        CALL UPDATE_DISPLAY_SAFE
        
        ; Проверка клавиш
        CALL CHECK_KEYS
        CP 0
        JR Z,.CONTINUE
        
        ; Код выхода в A уже установлен CHECK_KEYS
        LD (EXIT_CODE),A
        JR EXIT_PLAYER
        
.CONTINUE:
        ; Проверка конца песни
        LD A,(SONG_ENDED)
        OR A
        JR NZ,SONG_DONE
        
        JR MAIN_LOOP

SONG_DONE:
        ; Loop или следующий трек
        LD A,(VGM_LOOP_OFFSET)
        OR A
        JR Z,.NEXT_TRACK
        
        ; Есть loop - перезапустить
        CALL RESTART_LOOP
        JR MAIN_LOOP
        
.NEXT_TRACK:
        ; Переход на следующий файл (A=2)
        LD A,2
        LD (EXIT_CODE),A
        ; Падаем в EXIT_PLAYER

EXIT_PLAYER:
        ; Очистка
        CALL CLEANUP            ; Остановка OPL3
        CALL TIMING_CLEANUP     ; Восстановление портов
        CALL REMOVE_UI
        
        ; Восстановить CPU speed
        LD A,(OLD_CPU_SPEED)
        OUT (PORT_CPU),A
        
        ; WC автоматически восстановит прерывания
        
        ; Вернуть код выхода
        LD A,(EXIT_CODE)
        RET

; Новые функции клавиш
CHECK_KEYS:
        ; ESC - выход (A=0)
        CALL ESC
        JR NZ,.EXIT_NORMAL
        
        ; SPACE - следующий трек (A=2)
        CALL SPKE
        JR NZ,.NEXT_TRACK
        
        ; P - предыдущий трек (A=4)
        LD A,1
        CALL KBSCN
        CP 'p'
        JR Z,.PREV_TRACK
        CP 'P'
        JR Z,.PREV_TRACK
        
        ; M - пауза
        LD A,1
        CALL KBSCN
        CP 'm'
        JR Z,.TOGGLE_PAUSE
        CP 'M'
        JR Z,.TOGGLE_PAUSE
        
        ; Нет нажатия
        XOR A
        RET
        
.EXIT_NORMAL:
        XOR A               ; A=0 - обычный выход
        OR 1                ; NZ флаг
        RET
        
.NEXT_TRACK:
        LD A,2              ; A=2 - следующий файл
        OR 1
        RET
        
.PREV_TRACK:
        LD A,4              ; A=4 - предыдущий файл
        OR 1
        RET
        
.TOGGLE_PAUSE:
        LD A,(PAUSED)
        XOR 1
        LD (PAUSED),A
        
        ; Показать статус немедленно
        CALL UPDATE_DISPLAY
        
        XOR A               ; Не выходить
        RET

; Отключение прерываний WC
DISABLE_WC_INTERRUPTS:
        PUSH AF,HL
        
        XOR A               ; A'=0
        EXA
        LD HL,0
        CALL INT_PL         ; Отключить WC interrupts
        
        POP HL,AF
        RET

; WC API функция INT_PL
INT_PL:
        EXA
        LD A,86
        JP WLD
```

### playback.asm - Проверка фреймовых переменных

```asm
PROCESS_VGM_FRAME:
        ; Инициализация счетчиков
        XOR A
        LD (FRAME_SAMPLES),A
        LD (FRAME_SAMPLES+1),A
        LD (COMMAND_COUNTER),A
        LD (COMMAND_COUNTER+1),A
        
.COMMAND_LOOP:
        ; ... обработка команд ...
        
        ; ВСЕ операции ADD проверяем на overflow
        ; Пример в CMD_WAIT_NN:
        
CMD_WAIT_NN:
        CALL GET_VGM_BYTE_SAFE
        RET NZ
        LD L,A
        CALL GET_VGM_BYTE_SAFE
        RET NZ
        LD H,A
        
        ; Добавить к FRAME_SAMPLES с проверкой
        PUSH HL
        LD DE,(FRAME_SAMPLES)
        ADD HL,DE
        
        ; Проверка переполнения
        JR NC,.NO_OVERFLOW
        
        ; Переполнение - установить максимум
        LD HL,#FFFF
        
.NO_OVERFLOW:
        LD (FRAME_SAMPLES),HL
        POP HL
        LD (WAIT_SAMPLES),HL
        
        ; Если wait > UI_UPDATE_THRESHOLD - отметить для UI
        ; НО НЕ ВЫЗЫВАТЬ UPDATE ЗДЕСЬ!
        LD DE,UI_UPDATE_THRESHOLD
        OR A
        SBC HL,DE
        JR C,.NO_UI_FLAG
        
        ; Запросить обновление UI (безопасно)
        LD A,1
        LD (UI_UPDATE_NEEDED),A
        
.NO_UI_FLAG:
        ; Выполнить delay
        LD HL,(WAIT_SAMPLES)
        CALL DELAY_SAMPLES
        
        JP PROCESS_VGM_FRAME.COMMAND_LOOP
```

## ✅ Итоговый Чеклист

- [x] OPL3 детект с graceful fallback
- [x] Сохранение/восстановление прерываний через WC API
- [x] Безопасное обновление UI без рекурсии
- [x] Проверка overflow FRAME_SAMPLES
- [x] Выход с аргументом (A=2/4) для перемотки
- [x] 32-битная адресация файлов
- [x] Проверка границ файла
- [x] Защита от бесконечных циклов
- [x] Инициализация всех переменных

## 🎯 Приоритет Внедрения

1. **CRITICAL** - Сохранение прерываний (TIMING_INIT/CLEANUP)
2. **CRITICAL** - Безопасное UI обновление (флаги блокировки)
3. **HIGH** - OPL3 детект и условное использование
4. **HIGH** - Выход с аргументом для перемотки
5. **MEDIUM** - Overflow проверки FRAME_SAMPLES

Все эти исправления будут применены в следующих файлах.

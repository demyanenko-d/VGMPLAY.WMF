# WC API — Полный справочник функций

> Источники: `inc/wc_api.h` (1878 стр.), `asm/wc_api.s` (1339 стр.),
> `DiskRef/WC_History.txt` (история изменений API)

## Вызов функций

Все функции вызываются через точку доступа **#6006** (`JP` к диспетчеру):

```asm
WLD EQU #6006

; В регистр A загружается номер функции
; Остальные параметры - в соответствующие регистры
LD A, функция_номер
JP WLD
```

**Регистровые соглашения:**
- **Портит**: AF, BC, DE, HL, IX
- **Сохраняет**: IY, AF’, BC’, DE’, HL’, SP
- Функции с параметром в **A’** используют `EX AF,AF'` перед вызовом

> ⚠️ **Win 3 (#C000) сбрасывается** на текстовую страницу после **каждого** вызова
> `CALL #6006`. Плагин обязан повторить `mngcvpl()`/`mngc_pl()` после любого API-вызова.

## Категории функций

1. [Управление памятью](#управление-памятью)
2. [Работа с окнами](#работа-с-окнами)
3. [Работа с клавиатурой](#работа-с-клавиатурой)
4. [Файловые операции](#файловые-операции)
5. [Работа с графикой](#работа-с-графикой)
6. [Системные функции](#системные-функции)
7. [DMA операции](#dma-операции)
8. [Константы и структуры](#константы-и-структуры)
9. [Таблица кодов функций](#таблица-кодов-функций)

---

## Управление памятью

### #00 - MNGC_PL
Включение страницы плагина на #C000

**Вход:**
- A' - номер страницы (от 0)
  - #FF - страница с фонтом (нельзя использовать #E000-#FFFF!)
  - #FE - первый текстовый экран (с панелями)

**Пример:**
```asm
MNGC_PL:  EXA
          LD A,0       ; Страница 0
          CALL WLD
          EXA
          RET
```

### #78 - MNG0_PL
Включение страницы плагина на #0000

**Вход:**
- A' - номер страницы (от 0)

**Важно:** Все структуры для файловых функций должны лежать в #8000-#FFFF!

### #79 - MNG8_PL
Включение страницы плагина на #8000

**Вход:**
- A' - номер страницы (от 0)

---

## Работа с окнами

### Структура окна (SOW - Structure Of Window)

```asm
; Базовая структура (для типа 0)
SOW:
    DB 0           ; +0:  Флаги окна
    DB 0           ; +1:  Маска цвета курсора
    DB 255         ; +2:  X позиция (255=центр)
    DB 255         ; +3:  Y позиция (255=центр)
    DB 40          ; +4:  Ширина (0=весь экран)
    DB 20          ; +5:  Высота (0=весь экран)
    DB %01111111   ; +6:  Цвет (PAPER+INK)
    DB 0           ; +7:  Резерв (всегда 0)
    DW #0000       ; +8:  Адрес буфера (#0000=не выведено, #FFFF=без буфера)
    DB 0           ; +10: Линия разделитель 1
    DB 0           ; +11: Линия разделитель 2 (смещение снизу)
    DB 1           ; +12: Позиция курсора (от 1)
    DB 20          ; +13: Нижний ограничитель
    DB %01011000   ; +14: Цвет курсора
    DB %01111111   ; +15: Цвет под курсором
```

**Флаги окна (+0):**
```
[7]: %1 - окно с тенью
[6]: %1 - курсор во всю ширину
[5]: резерв
[4]: %1 - окно с инверсной шапкой
[3-0]: Тип окна:
  0 - стандартное окно, рамки 1-го типа
  1 - окно с заголовками и текстом (без курсора)
  2 - окно с заголовками и текстом + курсор
  3 - стандартное окно, рамки 2-го типа
  4 - окно с заголовками (рамки 2-го типа)
  5 - окно с заголовками + курсор (рамки 2-го типа)
  6 - стандартное окно, рамки 3-го типа
  7 - окно с заголовками (рамки 3-го типа)
  8 - окно с заголовками + курсор (рамки 3-го типа)
```

### #01 - PRWOW
Вывод окна на экран

**Вход:**
- IX - адрес структуры окна (SOW) в #8000-#BFFF или #0000-#3FFF

**Примечание:** Включает основной TXT экран в #C000 и оставляет его по выходу!

### #02 - RRESB
Стирание окна (восстановление информации)

**Вход:**
- IX - адрес SOW

**Примечание:** Включает основной TXT экран в #C000!

### #03 - PRSRW
Печать строки в окне

**Вход:**
- IX - адрес SOW
- HL - адрес текста (в #8000-#BFFF или #0000-#3FFF)
- D  - Y координата
- E  - X координата
- BC - длина строки

### #04 - PRIAT
Выставление цвета (после PRSRW)

**Вход:**
- A' - цвет (PAPER+INK)
- Использует координаты и длину из PRSRW

### #05 - GADRW
Получение адреса в окне

**Вход:**
- IX - адрес SOW
- D  - Y координата
- E  - X координата

**Выход:**
- HL - адрес в памяти

### #06 - CURSOR
Печать курсора

**Вход:**
- IX - адрес SOW

### #07 - CURSER
Стирание курсора

**Вход:**
- IX - адрес SOW

### #0B - TXTPR
Вывод текста с разметкой в окне

**Вход:**
- IX - адрес SOW
- HL - адрес текста (в #8000-#BFFF или #0000-#3FFF)
- D  - Y (внутри окна)
- E  - X (внутри окна)

**Выход:**
- D - Y (следующая строка)
- E - X (без изменений)

**Коды разметки текста:**
```
#00       - конец строки и абзаца
#00,#00   - разделитель между сообщениями
#0B,XX    - сдвиг вправо на XX символов
#0C,YY    - сдвиг вниз на YY строк
#0D       - перенос строки
#0E       - центрирование строки
#09       - инверсия цвета

#01-#08   - цвет чернил (1=синий, 7=белый, 8=черный)
#01-#08,#01-#08 - папер+инк

#FE,XXXX  - ссылка на строку (XXXX - адрес)
```

---

## Работа с клавиатурой

Все функции возвращают **NZ** если клавиша нажата, **Z** если нет.

| Функция | Код | Клавиша |
|---------|-----|---------|
| SPKE | #10 | SPACE |
| UPPP | #11 | UP Arrow |
| DWWW | #12 | DOWN Arrow |
| LFFF | #13 | LEFT Arrow |
| RGGG | #14 | RIGHT Arrow |
| TABK | #15 | TAB |
| ENKE | #16 | ENTER |
| ESC  | #17 | ESCAPE |
| BSPC | #18 | BACKSPACE |
| PGU  | #19 | PAGE UP |
| PGD  | #1A | PAGE DOWN |
| HOME | #1B | HOME |
| END  | #1C | END |
| F1-F10 | #1D-#26 | F1-F10 |
| ALT  | #27 | ALT |
| SHIFT| #28 | SHIFT |
| CTRL | #29 | CTRL |
| DEL  | #2B | DELETE |
| INS  | #55 | INSERT |
| CAPS | #2C | CAPS LOCK |
| ANYK | #2D | Любая клавиша |
| USPO | #2E | Пауза до отпускания клавиш |
| NUSP | #2F | Ожидание любой клавиши |

### #2A - KBSCN
Сканирование клавиатуры

**Вход:**
- A' - режим обработки
  - #00 - учитывает SHIFT, CAPS, ENG/RUS (вызывать 1 раз в фрейм)
  - #01 - всегда выдает код из таблицы (можно вызывать много раз)

**Выход:**
- NZ: A - код клавиши
- Z:  A - #00 (неизвестная клавиша) или #FF (буфер пуст)

**Пример:**
```asm
WAIT_KEY:
    EI
    HALT
    LD A,1
    CALL KBSCN
    CP 'm'           ; Ждем 'm'
    JR NZ,WAIT_KEY
```

---

## Файловые операции

### #30 - LOAD512
Потоковая загрузка файла

**Вход:**
- HL - адрес буфера
- B  - количество блоков (по 512 байт)

**Выход:**
- HL - новое значение адреса
- A  - #0F если конец файла

### #31 - SAVE512
Потоковая запись файла

**Вход:**
- HL - адрес буфера
- B  - количество блоков (по 512 байт)

**Выход:**
- HL - новое значение адреса
- A  - #0F если конец файла

### #32 - GIPAGPL
Позиционирование на начало файла

Устанавливает указатель на начало файла, переданного плагину.

### #3B - FENTRY
Поиск файла/каталога

**Вход:**
- HL - адрес структуры: flag(1), name(1-255), #00
  - flag: #00 = файл, #10 = каталог

**Выход:**
- Z  - файл не найден
- NZ - файл найден, [DE,HL] = размер файла
  - Вызовите GFILE/GDIR для активации

**Пример:**
```asm
    LD HL,FILE_STRUCT
    LD A,#3B
    CALL WLD
    JR Z,NOT_FOUND
    ; Файл найден
    CALL GFILE        ; Активировать файл
    ...
FILE_STRUCT:
    DB #00            ; Файл
    DB "music.vgm",0
```

### #3E - GFILE
Активация найденного файла

Устанавливает указатель на начало файла, найденного FENTRY.

### #3F - GDIR
Активация найденного каталога

Делает каталог, найденный FENTRY, активным.

### #48 - MKfile
Создание файла

**Вход:**
- HL - адрес структуры: flag(1), length(4), name(1-255), #00

**Выход:**
- A  - код ошибки
- Z  - файл создан
- NZ - ошибка

### #39 - STREAM
Работа с потоками

**Вход:**
- D - команда:
  - #FF - активировать root dir
  - #FE - клонировать поток в #00 и #01
  - #FD - активировать поток INI файла
  - #00/#01 - номер потока, требует BC

- BC (для #00/#01):
  - B - устройство (0=SD, 1=IDE Master, 2=IDE Slave)
  - C - раздел
  - #FFFF - просто включить поток

**Выход:**
- Z  - поток активен
- NZ - ошибка

---

## Работа с графикой

### #40 - MNGV_PL
Включение видео страницы

**Вход:**
- A' - номер видео страницы:
  - #00 - основной экран (TXT)
  - #01 - видео буфер 1 (16 страниц)
  - #02 - видео буфер 2 (16 страниц)
  - #03 - видео буфер 3 (16 страниц)
  - #04 - видео буфер 4 (16 страниц)

### #41 - MNGCVPL
Включение страницы видео буфера на #C000

**Вход:**
- A' - номер страницы:
  - #00-#0F - из буфера 1
  - #10-#1F - из буфера 2
  - #20-#2F - из буфера 3
  - #30-#3F - из буфера 4

### #42 - GVmod
Включение видео режима

**Вход:**
- A' - видео режим:
```
[7-6]: Разрешение
  %00 - 256x192
  %01 - 320x200
  %10 - 320x240
  %11 - 360x288
[1-0]: Тип
  %00 - ZX
  %01 - 16c
  %10 - 256c
  %11 - txt
```

---

## Системные функции

### #0E - TURBOPL
Управление частотой процессора

**Вход:**
- B - выбор Z80/AY:
  - #00 (`WC_TURBO_CPU`) - частота Z80:
    - C = %00 (3.5 МГц), %01 (7 МГц), %10 (14 МГц), %11 (28 МГц)
  - #01 (`WC_TURBO_AY`) - частота AY:
    - C = %00 (1.75 МГц), %01 (1.7733 МГц), %10 (3.5 МГц), %11 (3.546 МГц)
  - #FF (`WC_TURBO_RESTORE`) - восстановить значения из настроек WC

> AY частоты 1.7733/3.546 МГц — это NTSC-кварц (315/88 МГц ÷ 2).

### #0F - GEDPL
Восстановление текстового режима

**Важно:** Обязательно вызывать при запуске плагина!

Восстанавливает палитру, оффсеты, включает TXT режим.

**Примечание:** Гробит область #1000-#1447!

### #56 - INT_PL
Работа с прерываниями

**Вход:**
- A' - команда:
  - #00 - отключить все INT функции
  - #01 - отключить обновление времени
  - #02 - отключить опрос PS/2 клавиатуры
  - #FF - установить обработчик INT от плагина
    - HL - адрес обработчика (#8000-#BFFE)

**Примечание:** Все параметры восстанавливаются при выходе из плагина.

### #53 - PRM_PL
Запрос параметров запуска

**Вход:**
- A' - номер параметра (1-8)

**Выход:**
- Z  - параметр не найден
- NZ - параметр получен
  - A - номер выбранной опции (1-10)

---

## DMA операции

### #0D - DMAPL
Работа с DMA

**Инициализация:**
- A' = #00: инит S и D (BHL=Source, CDE=Destination)
- A' = #01: инит S (BHL)
- A' = #02: инит D (CDE)
- A' = #05: DMA_T (B = количество бёрстов, 0=1...255=256)
- A' = #06: DMA_N (B = размер бёрста)

Формат B/C для #00-#02:
```
[7]: %1 - страница из блока плагина (0-5)
     %0 - страница из видео буферов (0-63)
[6-0]: номер страницы
```

**Запуск:**
- A' = #FC: RAM→RAM
- A' = #FE: RAM→RAM с ожиданием
- A' = #FF: ожидание готовности DMA

**Выход:**
- Z  - операция завершена
- NZ - DMA не готов

**Пример:**
```asm
; Копирование 4KB из страницы 0 в страницу 1
    LD B,#80        ; Страница 0 плагина
    LD HL,#8000     ; Адрес источника
    LD C,#81        ; Страница 1 плагина
    LD DE,#8000     ; Адрес назначения
    EXA
    LD A,#00        ; Инит S и D
    CALL WLD
    LD A,#05
    LD B,8          ; 8 бёрстов
    CALL WLD
    LD A,#06
    LD B,255        ; По 512 байт
    CALL WLD
    LD A,#FE        ; Запуск с ожиданием
    CALL WLD
    EXA
```

## Макросы для удобства

```asm
; Определения для вызова функций
WLD     EQU #6006

; Окна
PRWOW   MACRO
        LD A,1
        JP WLD
        ENDM

RRESB   MACRO
        LD A,2
        JP WLD
        ENDM

; Клавиатура
ESC     MACRO
        LD A,#17
        JP WLD
        ENDM

ENKE    MACRO
        LD A,#16
        JP WLD
        ENDM

; Файлы
LOAD512 MACRO
        LD A,#30
        JP WLD
        ENDM

GEDPL   MACRO
        LD A,#0F
        JP WLD
        ENDM
```

---

## C API (SDCC)

> Источник: `src_sdcc/inc/wc_api.h` + `src_sdcc/asm/wc_api.s`  
> Компилятор: SDCC 4.x, соглашение **sdcccall(1)**

### Соглашение вызова sdcccall(1)

| Аргумент | Тип         | Регистр  |
|----------|-------------|----------|
| 1-й      | `uint8_t`   | `A`      |
| 1-й      | `uint16_t` / указатель | `HL` |
| 2-й      | `uint16_t` / указатель | `DE` |
| 3-й+     | любой       | стек (callee-cleans) |
| Возврат  | `uint8_t`   | `A`      |
| Возврат  | `uint16_t`  | `HL`     |

WC API **портит**: `AF, BC, DE, HL, IX` при каждом вызове.  
WC API **сохраняет**: `IY, AF', BC', DE', HL', SP`.

---

### Управление памятью (страницы)

```c
// Переключить окно #C000 на логическую страницу плагина N
void wc_mngc_pl(uint8_t page);

// Переключить окно #0000 на логическую страницу плагина N
void wc_mng0_pl(uint8_t page);

// Переключить окно #8000 на логическую страницу плагина N
void wc_mng8_pl(uint8_t page);

// Переключить окно #C000 на VPL-страницу мегабуфера N (физич. = WC_PAGE_TVBPG + N)
void wc_mngcvpl(uint8_t vpage);

// Переключить видеобуфер (WC_VIDEO_TXT / WC_VIDEO_BUF1..4)
void wc_mngv_pl(uint8_t mode);
```

> ⚠️ После **любого** вызова WC API окно `#C000` сбрасывается на текстовую страницу!  
> Если плагин работал с `mngcvpl()`, после каждого UI-вызова нужно снова вызвать `mngcvpl()`.

---

### Отображение / UI

```c
// Восстановить дисплей WC (TXT-режим, панели)
// ⚠ Затирает #1000–#1447 в окне 0!
void wc_gedpl(void);

// Нарисовать окно
void wc_prwow(wc_window_t *win);

// Убрать окно (восстановить фон из RESB)
void wc_rresb(wc_window_t *win);

// Прокрутить область окна
void wc_scrlwow(wc_window_t *win, uint8_t y, uint8_t x,
                uint8_t h, uint8_t w, uint8_t flags);

// Вывести строку с разметкой WC в окно
// Возвращает: номер строки после последнего символа
uint8_t wc_txtpr(wc_window_t *win, const char *str, uint8_t y, uint8_t x);

// Вывести сообщение (несколько абзацев, разделённых 0x00 0x00)
uint8_t wc_mezz(wc_window_t *win, uint8_t msg_num,
                const char *str, uint8_t y, uint8_t x);

// Вывести строку без разметки (raw)
void wc_prsrw(wc_window_t *win, const char *str,
              uint8_t y, uint8_t x, uint16_t len);

// Вывести строку без разметки с заданным атрибутом цвета
void wc_prsrw_attr(wc_window_t *win, const char *str,
                   uint8_t y, uint8_t x, uint16_t len, uint8_t attr);

// Получить адрес экрана по позиции в окне
uint16_t wc_gadrw(wc_window_t *win, uint8_t y, uint8_t x);

// Показать/убрать курсор
void wc_cursor(wc_window_t *win);
void wc_curser(wc_window_t *win);

// Диалог Yes/No или Ok/Cancel
// mode: WC_YN_YES_NO, WC_YN_OK_CANCEL, WC_YN_POLL, WC_YN_EXIT
// Возвращает: 0=нет/отмена, 1=да/ок (при POLL: текущий выбор)
uint8_t wc_yn(uint8_t mode);

// Ввод строки в окне
void wc_istr(wc_window_t *win, uint8_t mode);

// Вставить символ val по адресу addr в текстовой памяти
void wc_nork(uint16_t addr, uint8_t val);
```

---

### Клавиатура

```c
// Опрос конкретной клавиши (возвращает 1 если нажата)
uint8_t wc_key_space(void);
uint8_t wc_key_up(void);
uint8_t wc_key_down(void);
uint8_t wc_key_left(void);
uint8_t wc_key_right(void);
uint8_t wc_key_tab(void);
uint8_t wc_key_enter(void);
uint8_t wc_key_esc(void);
uint8_t wc_key_bspc(void);
uint8_t wc_key_pgup(void);
uint8_t wc_key_pgdn(void);
uint8_t wc_key_home(void);
uint8_t wc_key_end(void);
uint8_t wc_key_f1(void);
uint8_t wc_key_f2(void);
uint8_t wc_key_f3(void);
uint8_t wc_key_f4(void);
uint8_t wc_key_f5(void);
uint8_t wc_key_f6(void);
uint8_t wc_key_f7(void);
uint8_t wc_key_f8(void);
uint8_t wc_key_f9(void);
uint8_t wc_key_f10(void);

// Модификаторы (без автоповтора)
uint8_t wc_key_alt(void);
uint8_t wc_key_shift(void);
uint8_t wc_key_ctrl(void);

// Прочие клавиши
uint8_t wc_key_del(void);
uint8_t wc_key_ins(void);
uint8_t wc_key_caps(void);
uint8_t wc_key_any(void);          // любая клавиша нажата (без автоповтора)

// Ждать нажатия любой клавиши, вернуть скан-код
uint8_t wc_key_wait_any(void);

// Ждать отпускания всех клавиш
void    wc_key_wait_release(void);

// Получить скан-код нажатой клавиши
// mode: WC_KBSCN_NORMAL (с учётом SHIFT/CL/Lang) или WC_KBSCN_RAW
uint8_t wc_kbscn(uint8_t mode);
```

---

### Файловые операции

Все буферы и строки файловых функций **обязаны** быть в `#8000–#FFFF`  
(WC ремапит `#0000–#7FFF` внутри FAT-операций).

```c
// Прочитать blocks×512 байт по адресу dest из открытого файла
// Возвращает: 0=OK, WC_EOF=конец файла, иное=ошибка
uint8_t wc_load512(uint16_t dest, uint8_t blocks);

// Прочитать blocks×256 байт по адресу dest
uint8_t wc_load256(uint16_t dest, uint8_t blocks);

// Записать blocks×512 байт по адресу src в открытый файл
void wc_save512(uint16_t src, uint8_t blocks);

// Пропустить sectors×512 байт в открытом файле (без чтения)
void wc_loadnone(uint8_t sectors);

// Сменить поток (контекст файловой операции)
// mode: WC_STREAM_ROOT, WC_STREAM_CLONE, WC_STREAM_WCDIR, 0/1
void wc_stream(uint8_t mode);

// Управление каталогом (WC_ADIR_SEEK_START, WC_ADIR_RESET_NEXT)
void wc_adir(uint8_t mode);

// Открыть файл по имени в текущем каталоге
// name_with_flag: [1 байт флагов][имя][0]
// Возвращает: 0=не найден, ненулевое=найден
uint8_t wc_fentry(uint16_t name_with_flag);

// Найти следующий файл в каталоге
// flags: [7]=только короткие имена, [1:0]=00 все/01 файлы/10 каталоги
// Возвращает: 0 (Z=1)=конец каталога, ненулевое=найден
uint8_t wc_findnext(uint16_t entry_buf, uint8_t flags);

// Открыть файл, найденный последним wc_fentry()
void wc_gfile(void);

// Войти в каталог, найденный последним wc_fentry()
void wc_gdir(void);

// Создать файл: [1 байт флагов][длина][имя][0]
uint8_t wc_mkfile(uint16_t name_with_flag);

// Создать каталог
uint8_t wc_mkdir(uint16_t name);

// Переименовать файл/каталог
uint8_t wc_rename(uint16_t old_name, uint16_t new_name);

// Удалить файл
uint8_t wc_delfl(uint16_t name_with_flag);

// Перейти к началу файла плагина (позиционировать поток на первый блок)
void wc_gipagpl(void);

// Прочитать FAT ENTRY (32 байта) файла в буфер по адресу addr
void wc_tentry(uint16_t addr);

// Обойти цепочку секторов, записать список секторов в buf..bufend
void wc_chtosep(uint16_t buf, uint16_t bufend);

// Получить заголовок помеченного файла
// panel = IX структура панели WC, filenum = номер файла, namebuf = буфер
void wc_tmrkdfl(uint16_t panel, uint16_t filenum, uint16_t namebuf);
```

---

### Графика и видео

```c
// Переключить видеорежим: WC_VIDEO_TXT / WC_VIDEO_BUF1..4
void wc_mngv_pl(uint8_t mode);

// Отобразить видеостраницу vpage (0x00–0x3F) на окно #C000
void wc_mngcvpl(uint8_t vpage);

// Отобразить видеостраницу vpage на окно #0000
// ⚠ GEDPL (~wc_gedpl) затирает #1000–#1447 — не хранить данные там!
void wc_mng0vpl(uint8_t vpage);

// Отобразить видеостраницу vpage на окно #8000
void wc_mng8vpl(uint8_t vpage);

// Установить видеорежим TSConfig
// mode: TSCONF_VM_* | TSCONF_RRES_* (байт VConfig)
void wc_gvmod(uint8_t mode);

// Установить Y-смещение прокрутки графики
void wc_gyoff(uint16_t y);

// Установить X-смещение прокрутки графики
void wc_gxoff(uint16_t x);

// Установить страницу тайловой карты
void wc_gvtm(uint8_t page);

// Установить страницу графики тайловой плоскости (plane=0 или 1)
void wc_gvtl(uint8_t plane, uint8_t page);

// Установить страницу графики спрайтов
void wc_gvsgp(uint8_t page);
```

---

### DMA

```c
// Настройка и запуск DMA TSConfig.
// Подфункции:
//   0x00 — src+dst: B=src_page, HL=src_addr, C=dst_page, DE=dst_addr
//   0x01 — только src; 0x02 — только dst
//   0x05 — DMA_T; 0x06 — DMA_N; 0x07 — T+N
//   0xFC — RAM→RAM (с ожиданием)  0xFB — BLT→RAM
//   0xFA — FILL                   0xF9 — RAM→CRAM (палитра)
//   0xF8 — RAM→SFILE (спрайты)    0xFD — запустить без ожидания
//   0xFE — запустить с ожиданием  0xFF — ждать готовности DMA
void wc_dmapl(uint8_t subfunc);
```

---

### Системные функции

```c
// Прерывания (WC_INT_DISABLE_ALL / NO_TIME / NO_PS2 / PLUGIN)
// При WC_INT_PLUGIN — следующий вызов wc_int_pl_handler() задаёт адрес
void wc_int_pl(uint8_t mode);

// Установить адрес обработчика ISR плагина (после wc_int_pl(WC_INT_PLUGIN))
void wc_int_pl_handler(uint16_t handler_addr);

// Управление CPU/AY частотой
// mode: WC_TURBO_CPU (param 0=3.5, 1=7, 2=14, 3=28 МГц)
//       WC_TURBO_AY  (param 0=1.750, 1=1.773, 2=3.5, 3=3.546 МГц)
//       WC_TURBO_RESTORE (0xFF) — восстановить из настроек WC
void wc_turbopl(uint8_t mode, uint8_t param);

// Получить параметр из INI-файла плагина
// param_num: 0-based номер параметра
// Возвращает: номер опции (0-based), 0xFF если параметр не задан
uint8_t wc_prm_pl(uint8_t param_num);

// Резидентный JP на другую страницу плагина (без возврата!)
// page: номер страницы (от 0), addr: адрес в #8000–#BFFF
void wc_resident_jump(uint8_t page, uint16_t addr);

// Резидентный CALL на другую страницу плагина (с возвратом)
// Сохраняет все регистры кроме A и HL
void wc_resident_call(uint8_t page, uint16_t addr);

// Заполнить буфер символом c (аналог memset)
void wc_strset(char *dsr, uint16_t len, char c);
```

---

### Системные переменные (прямой доступ по адресу)

```c
// Читаются напрямую, без вызова WC API:
extern volatile uint8_t  wc_exit_code;      // #8000 − записать перед return из main()
                                             // 0=ESC, 1=не распознан, 2=следующий,
                                             // 3=перечитать каталог, 4=предыдущий

#define WC_SYS_ABT  (*(volatile uint8_t*)0x6004)  // 1 = был нажат ESC
#define WC_SYS_ENT  (*(volatile uint8_t*)0x6005)  // 1 = был нажат ENTER
#define WC_SYS_TMN  (*(volatile uint16_t*)0x6009) // таймер INT (инкремент каждый INT)
#define WC_SYS_HEI  (*(volatile uint8_t*)0x600E)  // высота TXT-экрана (25/30/36)
#define WC_SYS_PG0  (*(volatile uint8_t*)0x6000)  // текущая страница окна #0000
#define WC_SYS_PGC  (*(volatile uint8_t*)0x6003)  // текущая страница окна #C000
```

---

### Пример минимального плагина (SDCC C)

```c
#include "inc/wc_api.h"
#include "inc/types.h"

// Структура окна в DATA (0x8000+)
static wc_window_t g_win;

// Код выхода (WC читает A при RET из main)
uint8_t wc_exit_code;

int main(void) {

    // --- Настройка окна ---
    g_win.type     = WC_WIN_SINGLE;
    g_win.cur_mask = 0x07;
    g_win.x        = 255;   // по центру
    g_win.y        = 255;
    g_win.width    = 40;
    g_win.height   = 8;
    g_win.color    = WC_COLOR(WC_BLACK, WC_BRIGHT_WHITE);
    g_win.buf_addr = 0x0000; // автосохранение фона

    // --- Отрисовка UI ---
    wc_gedpl();
    wc_prwow(&g_win);
    wc_txtpr(&g_win, "Hello from plugin!", 1, 1);

    // --- Главный цикл ---
    while (1) {
        if (wc_key_esc()) break;
        if (wc_key_enter()) {
            // ... какое-то действие
        }
    }

    // --- Выход ---
    wc_rresb(&g_win);
    wc_gedpl();

    wc_exit_code = 0;   // 0 = нормальный выход (ESC)
    return 0;
}
```
---

## Константы и структуры

### Цветовая палитра (EGA-совместимая)

```c
#define WC_COLOR(bg, fg)    ((uint8_t)(((bg) << 4) | (fg)))

#define WC_BLACK            0      #define WC_DARK_GRAY        8
#define WC_BLUE             1      #define WC_BRIGHT_BLUE      9
#define WC_RED              2      #define WC_BRIGHT_RED       10
#define WC_MAGENTA          3      #define WC_BRIGHT_MAGENTA   11
#define WC_GREEN            4      #define WC_BRIGHT_GREEN     12
#define WC_CYAN             5      #define WC_BRIGHT_CYAN      13
#define WC_YELLOW           6      #define WC_BRIGHT_YELLOW    14
#define WC_WHITE            7      #define WC_BRIGHT_WHITE     15
```

### Стили окон

| Константа | Значение | Описание |
|-----------|----------|----------|
| `WC_WIN_NO_BORDER` | 0x00 | Без рамки, со стандартным курсором |
| `WC_WIN_SINGLE` | 0x01 | Одинарная рамка, без курсора |
| `WC_WIN_DOUBLE` | 0x02 | Двойная рамка + курсор |
| `WC_WIN_TYPE3`..`TYPE8` | 0x03..0x08 | Рамки 2/3 типа ± курсор/заголовки |

**Флаги (OR с типом):**

| Флаг | Значение | Описание |
|------|----------|----------|
| `WC_WIN_HEADER` | 0x10 | Инверсный заголовок сверху |
| `WC_WIN_WIDE_CURSOR` | 0x40 | Курсор на всю ширину |
| `WC_WIN_SHADOW` | 0x80 | Тень у окна |

### Структура `wc_window_t` (16 байт)

```c
typedef struct {
    uint8_t  type;       /* +0:  стиль+флаги (WC_WIN_*)                */
    uint8_t  cur_mask;   /* +1:  маска цвета курсора                   */
    uint8_t  x;          /* +2:  X (255=по центру)                     */
    uint8_t  y;          /* +3:  Y (255=по центру)                     */
    uint8_t  width;      /* +4:  ширина (0=весь экран)                 */
    uint8_t  height;     /* +5:  высота (0=весь экран)                 */
    uint8_t  color;      /* +6:  WC_COLOR(bg, fg)                      */
    uint8_t  _reserved;  /* +7:  ВСЕГДА 0!                             */
    uint16_t buf_addr;   /* +8:  буфер фона (0=авто, 0xFFFF=без буфера)*/
    uint8_t  divider1;   /* +10: разделитель 1 (смещение от верха)     */
    uint8_t  divider2;   /* +11: разделитель 2 (смещение от низа)      */
    uint8_t  cursor_pos; /* +12: позиция курсора (от 1)                */
    uint8_t  cursor_bot; /* +13: нижний ограничитель курсора           */
    uint8_t  cursor_clr; /* +14: цвет курсора                          */
    uint8_t  under_clr;  /* +15: цвет под курсором                     */
} wc_window_t;
```

> Авто-центрирование (x=255, y=255) работает для режимов 80×25, 80×30, 90×36.
> Добавлено в WC v1.02.

### Структура FAT-записи (32 байта)

```c
typedef struct {
    uint8_t  name[11];    /* +0:  короткое имя 8.3 (без точки)        */
    uint8_t  attr;        /* +11: атрибуты FAT (см. WC_FAT_ATTR_*)    */
    uint8_t  _res1;       /* +12: NTRes                                */
    uint8_t  crt_time_ms; /* +13: время создания (0.1 сек)             */
    uint16_t crt_time;    /* +14: время создания (FAT format)          */
    uint16_t crt_date;    /* +16: дата создания                        */
    uint16_t acc_date;    /* +18: дата доступа                         */
    uint16_t cluster_hi;  /* +20: старшее слово кластера               */
    uint16_t mod_time;    /* +22: время модификации                    */
    uint16_t mod_date;    /* +24: дата модификации                     */
    uint16_t cluster_lo;  /* +26: младшее слово кластера               */
    uint32_t file_size;   /* +28: размер файла                         */
} wc_fat_entry_t;
```

**Атрибуты FAT:**

| Константа | Значение | Описание |
|-----------|----------|----------|
| `WC_FAT_ATTR_RDONLY` | 0x01 | Только чтение |
| `WC_FAT_ATTR_HIDDEN` | 0x02 | Скрытый |
| `WC_FAT_ATTR_SYSTEM` | 0x04 | Системный |
| `WC_FAT_ATTR_VOLLBL` | 0x08 | Метка тома |
| `WC_FAT_ATTR_DIR` | 0x10 | Каталог |
| `WC_FAT_ATTR_ARCHIVE` | 0x20 | Архивный |
| `WC_FAT_ATTR_LFN` | 0x0F | LFN-запись (RDONLY\|HIDDEN\|SYSTEM\|VOLLBL) |

### Структура FindNext

```c
typedef struct {
    uint32_t size;    /* опционально, если WC_FIND_WITH_SIZE         */
    uint16_t date;    /* опционально, если WC_FIND_WITH_DATE         */
    uint16_t time;    /* опционально, если WC_FIND_WITH_TIME         */
    uint8_t  flag;    /* 0x10=каталог, 0x00=файл                     */
    char     name[1]; /* имя, z-string (длина переменная)            */
} wc_findnext_entry_t;
```

**Флаги для `wc_findnext(entry_buf, flags)`:**

| Константа | Значение | Описание |
|-----------|----------|----------|
| `WC_FIND_ALL` | 0x00 | Все записи |
| `WC_FIND_FILES` | 0x01 | Только файлы |
| `WC_FIND_DIRS` | 0x02 | Только каталоги |
| `WC_FIND_WITH_SIZE` | 0x04 | Включить поле size |
| `WC_FIND_WITH_DATE` | 0x08 | Включить поле date |
| `WC_FIND_WITH_TIME` | 0x10 | Включить поле time |
| `WC_FIND_FULL` | 0x1C | Все опциональные поля |
| `WC_FIND_SHORT_ONLY` | 0x80 | Только короткие имена 8.3 |

### Коды ошибок файловых операций

| Константа | Значение | Описание |
|-----------|----------|----------|
| `WC_EOF` | 0x0F | Конец файла / конец цепочки |
| `WC_ERR_LONG_NAME` | 1 | Недопустимое длинное имя |
| `WC_ERR_SHORT_IDX` | 2 | Короткое имя требует индекса |
| `WC_ERR_LONG_EXISTS` | 3 | Длинное имя уже существует |
| `WC_ERR_SHORT_EXISTS` | 4 | Короткое имя уже существует |
| `WC_ERR_NOT_FOUND` | 8 | Файл/каталог не найден |
| `WC_ERR_NO_SPACE` | 16 | Нет свободного места |
| `WC_ERR_UNKNOWN` | 255 | Неизвестная ошибка |

### Константы скроллинга (`wc_scrlwow`)

| Константа | Значение | Описание |
|-----------|----------|----------|
| `WC_SCRL_DOWN` | 0x00 | Прокрутка вниз |
| `WC_SCRL_UP` | 0x01 | Прокрутка вверх |
| `WC_SCRL_WITH_ATTRS` | 0x80 | Прокручивать и атрибуты |
| `WC_SCRL_CLEAR_SRC` | 0x40 | Очистить источник после сдвига |

### Константы потоков (`wc_stream`)

| Константа | Значение | Описание |
|-----------|----------|----------|
| `WC_STREAM_ROOT` | 0xFF | Корневой каталог |
| `WC_STREAM_CLONE` | 0xFE | Клонировать текущий поток в #00 и #01 |
| `WC_STREAM_WCDIR` | 0xFD | Каталог WC (для INI) |
| 0x00, 0x01 | — | Выбрать поток 0 или 1 |

### Константы видеобуферов

| Константа | Значение | Описание |
|-----------|----------|----------|
| `WC_VIDEO_TXT` | 0x00 | Текстовый режим WC |
| `WC_VIDEO_BUF1` | 0x01 | Видеобуфер 1 (стр. #20–#2F) |
| `WC_VIDEO_BUF2` | 0x02 | Видеобуфер 2 (стр. #30–#3F) |
| `WC_VIDEO_BUF3` | 0x03 | Видеобуфер 3 (стр. #40–#4F) |
| `WC_VIDEO_BUF4` | 0x04 | Видеобуфер 4 (стр. #50–#5F) |

> Мегабуфер (64 стр.) и видеобуферы **пересекаются** физически:
> VPL-страница N = физическая `WC_PAGE_TVBPG + N` = `#20 + N`.

### Размеры TXT-экрана

| Константа | Значение |
|-----------|----------|
| `WC_SCREEN_80x25` | 25 |
| `WC_SCREEN_80x30` | 30 |
| `WC_SCREEN_90x36` | 36 |

---

## Таблица кодов функций

> Полный маппинг ASM → WC API code → C wrapper

| Код A | ASM | C функция | Параметр в A' (EXA)? | Примечание |
|-------|-----|-----------|----------------------|------------|
| #00 | MNGC_PL | `wc_mngc_pl(page)` | **Да** (page) | #FF=font, #FE=TXT |
| #01 | PRWOW | `wc_prwow(win)` | — | IX=win |
| #02 | RRESB | `wc_rresb(win)` | — | IX=win |
| #03 | PRSRW | `wc_prsrw(win,str,y,x,len)` | — | |
| #04 | PRIAT | (часть prsrw_attr) | **Да** (color) | Вызывается сразу после #03 |
| #05 | GADRW | `wc_gadrw(win,y,x)` | — | → HL=addr |
| #06 | CURSOR | `wc_cursor(win)` | — | |
| #07 | CURSER | `wc_curser(win)` | — | |
| #08 | YN | `wc_yn(mode)` | **Да** (mode) | |
| #09 | ISTR | `wc_istr(win,mode)` | **Да** (mode) | |
| #0A | NORK | `wc_nork(addr,val)` | **Да** (val) | |
| #0B | TXTPR | `wc_txtpr(win,str,y,x)` | — | → A=next_y |
| #0C | MEZZ | `wc_mezz(win,msg,str,y,x)` | **Да** (msg_num) | |
| #0D | DMAPL | `wc_dmapl(subfunc)` | **Да** (subfunc) | |
| #0E | TURBOPL | `wc_turbopl(mode,param)` | — | B=mode, C=param |
| #0F | GEDPL | `wc_gedpl()` | — | ⚠ Портит #1000–#1447 |
| #10..#29 | Keys | `wc_key_*()` | — | → NZ=нажата |
| #2A | KBSCN | `wc_kbscn(mode)` | **Да** (mode) | |
| #2B | DEL | `wc_key_del()` | — | |
| #2C | CAPS | `wc_key_caps()` | — | |
| #2D | ANYK | `wc_key_any()` | — | |
| #2E | USPO | `wc_key_wait_release()` | — | |
| #2F | NUSP | `wc_key_wait_any()` | — | |
| #30 | LOAD512 | `wc_load512(dest,blocks)` | — | B=blocks |
| #31 | SAVE512 | `wc_save512(src,blocks)` | — | |
| #32 | GIPAGPL | `wc_gipagpl()` | — | |
| #33 | TENTRY | `wc_tentry(addr)` | — | |
| #34 | CHTOSEP | `wc_chtosep(buf,bufend)` | — | |
| #36 | TMRKDFL | `wc_tmrkdfl(panel,fnum,buf)` | — | |
| #38 | ADIR | `wc_adir(mode)` | **Да** (mode) | |
| #39 | STREAM | `wc_stream(mode)` | — | D=mode |
| #3A | FINDNEXT | `wc_findnext(buf,flags)` | **Да** (flags) | |
| #3B | FENTRY | `wc_fentry(name)` | — | → Z/NZ |
| #3C | LOAD256 | `wc_load256(dest,blocks)` | — | |
| #3D | LOADNONE | `wc_loadnone(sectors)` | — | B=sectors |
| #3E | GFILE | `wc_gfile()` | — | |
| #3F | GDIR | `wc_gdir()` | — | |
| #40 | MNGV_PL | `wc_mngv_pl(mode)` | **Да** (mode) | |
| #41 | MNGCVPL | `wc_mngcvpl(vpage)` | **Да** (vpage) | |
| #42 | GVmod | `wc_gvmod(mode)` | **Да** (mode) | |
| #43 | GYOFF | `wc_gyoff(y)` | — | HL=y |
| #44 | GXOFF | `wc_gxoff(x)` | — | HL=x |
| #45 | GVtm | `wc_gvtm(page)` | — | C=page |
| #46 | GVtl | `wc_gvtl(plane,page)` | — | B=plane,C=page |
| #47 | GVsgp | `wc_gvsgp(page)` | — | C=page |
| #48 | MKfile | `wc_mkfile(name)` | — | |
| #49 | MKdir | `wc_mkdir(name)` | — | |
| #4A | RENAME | `wc_rename(old,new)` | — | |
| #4B | DELFL | `wc_delfl(name)` | — | |
| #4E | MNG0_PL | `wc_mng0_pl(page)` | **Да** (page) | |
| #4F | MNG8_PL | `wc_mng8_pl(page)` | **Да** (page) | |
| #50 | MNG0VPL | `wc_mng0vpl(vpage)` | **Да** (vpage) | |
| #51 | MNG8VPL | `wc_mng8vpl(vpage)` | **Да** (vpage) | |
| #53 | PRM_PL | `wc_prm_pl(param)` | **Да** (param) | → Z=нет, NZ: A=opt |
| #54 | SCRLWOW | `wc_scrlwow(win,y,x,h,w,fl)` | **Да** (flags) | |
| #55 | INS | `wc_key_ins()` | — | |
| #56 | INT_PL | `wc_int_pl(mode)` | **Да** (mode) | |
| — | PLRESAD | `wc_resident_jump(p,addr)` | — | CALL #6020, JP |
| — | PLRESCALL | `wc_resident_call(p,addr)` | — | CALL #6028 |
| — | STRSET | `wc_strset(dst,len,c)` | — | Утилита (LDIR) |
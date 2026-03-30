# TSConfig Platform Reference

> Источники: `inc/tsconf.h` (532 стр.), `inc/wc_api.h` (системные константы),
> [TSConfig Wiki](https://github.com/tslabs/zx-evo)

## Обзор платформы

TS-Configuration (TSConfig) — расширенная конфигурация на базе **ZX Evolution**
(Большой Спектрум / ZX Evo), реализованная в FPGA (Altera/Xilinx).

| Параметр | Значение |
|-----------|----------|
| CPU | Z80 @ 3.5 / 7 / 14 / 28 МГц |
| RAM | до 4096 КБ (256 страниц × 16 КБ) |
| ROM | до 512 КБ (32 страницы) |
| Видео | до 360×288, 256 цветов, TXT 80/90 столбцов |
| Спрайты | До 85 на строку, 8×8–64×64, 3 плоскости |
| Тайлы | 2 плоскости, 8×8 |
| DMA | Аппаратный контроллер (RAM↔RAM, RAM↔SPI/IDE, FILL, CRAM, SFILE) |
| Кэш | 512 байт, по-оконно |
| Звук | AY-3-8910 (TurboSound: 2 чипа), OPL3 (YMF262), SAA1099 ×2, HUS |
| Хранилище | SD (Z-Controller, NeoGS), IDE (NEMO, SMUC), FAT32 |
| Совместимость | Pentagon-128/512/1024, порт #7FFD |

## Память

### Страничная адресация

Процессор Z80 адресует 64 КБ памяти, разделенные на 4 окна по 16 КБ:

| Окно | Адреса CPU | Регистр страницы | R/W | Reset |
|------|------------|------------------|-----|-------|
| 0 | #0000..#3FFF | Page0 | W | #00 |
| 1 | #4000..#7FFF | Page1 | W | #05 |
| 2 | #8000..#BFFF | Page2 | R/W | #02 |
| 3 | #C000..#FFFF | Page3 | R/W | #00 |

**Доступно:**
- RAM: до 4096 КБ (256 страниц по 16 КБ)
- ROM: до 512 КБ (32 страницы по 16 КБ)

### Регистры управления памятью

Доступ к регистрам через порты **#nnAF**, где nn - номер регистра.

#### MemConfig (регистр #07)

```
Биты:
7:6 - LCK128[1:0] - режим порта #7FFD
5:4 - резерв
3   - W0_RAM - выбор ROM/RAM в окне 0
2   - !W0_MAP - режим маппинга окна 0
1   - W0_WE - разрешение записи в окно 0
0   - ROM128 - выбор страниц маппинга
```

#### Page0..3 (регистры #10-#13)

8-битные регистры, содержащие номер страницы для соответствующего окна.

## Порты ввода-вывода

### Схема адресации портов

TSConfig использует порты с адресом **#nnAF**, где:
- Младшие 8 бит (A0-A7) = #AF
- Старшие 8 бит (A8-A15) = номер регистра (nn)

### Базовые регистры TSConfig

> Источник: `src_sdcc/inc/tsconf.h` — `#define TSCONF_PORT(reg) (((uint16_t)(reg) << 8) | 0xAF)`

| Порт    | Константа                  | R/W | Описание                            |
|---------|----------------------------|-----|-------------------------------------|
| `#00AF` | `TSCONF_PORT_VCONFIG`      | RW  | VConfig: видеорежим (VM, RRES)      |
| `#01AF` | `TSCONF_PORT_VPAGE`        | W   | VPage: страница видеобуфера         |
| `#02AF` | `TSCONF_PORT_GXOFFSL`      | W   | GFX X offset (low)                  |
| `#03AF` | `TSCONF_PORT_GXOFFSH`      | W   | GFX X offset (high)                 |
| `#04AF` | `TSCONF_PORT_GYOFFSL`      | W   | GFX Y offset (low)                  |
| `#05AF` | `TSCONF_PORT_GYOFFSH`      | W   | GFX Y offset (high)                 |
| `#06AF` | `TSCONF_PORT_TSCONFIG`     | W   | TSConfig: tile/sprite engine        |
| `#07AF` | `TSCONF_PORT_PALSEL`       | W   | PalSel: выбор палитры               |
| `#0FAF` | `TSCONF_PORT_BORDER`       | W   | Border: цвет бордюра (биты 2:0)    |
| `#10AF` | `TSCONF_PORT_PAGE0`        | **W** | **Page0: страница окна #0000**    |
| `#11AF` | `TSCONF_PORT_PAGE1`        | **W** | **Page1: страница окна #4000**    |
| `#12AF` | `TSCONF_PORT_PAGE2`        | **RW** | **Page2: страница окна #8000**   |
| `#13AF` | `TSCONF_PORT_PAGE3`        | **RW** | **Page3: страница окна #C000**   |
| `#14AF` | `TSCONF_PORT_FMADDR`       | W   | Font Maps                           |
| `#15AF` | `TSCONF_PORT_TMPAGE`       | W   | TileMap page                        |
| `#16AF` | `TSCONF_PORT_T0GPAGE`      | W   | Tile plane 0 gfx page              |
| `#17AF` | `TSCONF_PORT_T1GPAGE`      | W   | Tile plane 1 gfx page              |
| `#18AF` | `TSCONF_PORT_SGPAGE`       | W   | Sprite gfx page                    |
| `#20AF` | `TSCONF_PORT_SYSCONFIG`    | W   | **SysConfig: CPU freq + кэш**      |
| `#21AF` | `TSCONF_PORT_MEMCONFIG`    | W   | MemConfig: режим памяти             |
| `#22AF` | `TSCONF_PORT_HSINT`        | W   | FRAME-INT: горизонт. позиция       |
| `#23AF` | `TSCONF_PORT_VSINTL`       | W   | FRAME-INT: вертик. позиция (low)   |
| `#24AF` | `TSCONF_PORT_VSINTH`       | W   | FRAME-INT: вертик. позиция (high) + auto-inc |
| `#29AF` | `TSCONF_PORT_FDDVIRT`      | W   | FDD виртуализация                   |
| `#2AAF` | `TSCONF_PORT_INTMASK`      | W   | **INTMask: маска прерываний**       |
| `#2BAF` | `TSCONF_PORT_CACHECONFIG`  | W   | **CacheConfig: кэш по окнам**      |

#### Порты тайловых смещений

| Порт | Описание |
|------|----------|
| `#40AF`/`#41AF` | Tile plane 0: X offset (lo/hi) |
| `#42AF`/`#43AF` | Tile plane 0: Y offset (lo/hi) |
| `#44AF`/`#45AF` | Tile plane 1: X offset (lo/hi) |
| `#46AF`/`#47AF` | Tile plane 1: Y offset (lo/hi) |

#### Порты DMA

| Порт | Константа | Описание |
|------|-----------|----------|
| `#19AF` | `TSCONF_PORT_DMASADDRL` | Source addr low |
| `#1AAF` | `TSCONF_PORT_DMASADDRH` | Source addr high |
| `#1BAF` | `TSCONF_PORT_DMASADDRX` | Source page (extended) |
| `#1CAF` | `TSCONF_PORT_DMADADDRL` | Dest addr low |
| `#1DAF` | `TSCONF_PORT_DMADADDRH` | Dest addr high |
| `#1EAF` | `TSCONF_PORT_DMADADDRX` | Dest page (extended) |
| `#25AF` | `TSCONF_PORT_DMAWPDEV` | Wait Port device |
| `#26AF` | `TSCONF_PORT_DMALEN` | Burst length |
| `#27AF` | `TSCONF_PORT_DMACTRL` | Control (W) / Status (R) |
| `#28AF` | `TSCONF_PORT_DMANUM` | Bursts num (low) |
| `#2CAF` | `TSCONF_PORT_DMANUMH` | Bursts num (high) |
| `#2DAF` | `TSCONF_PORT_DMAWPADDR` | Wait Port addr |

> ⚠️ **Частая ошибка:** Page0–Page3 = `#10AF`–`#13AF`, SysConfig = `#20AF`.
> Не путать с VConfig (`#00AF`) или VPage (`#01AF`)!

### Порты OPL3 — YMF262

**OPL3 чип располагается по портам #C4–#C7:**

| Порт  | Константа | Назначение |
|-------|-----------|------------|
| `#C4` | `TSCONF_PORT_OPL3_ADDR0` | Bank 0: регистр адреса |
| `#C5` | `TSCONF_PORT_OPL3_DATA0` | Bank 0: данные |
| `#C6` | `TSCONF_PORT_OPL3_ADDR1` | Bank 1: регистр адреса |
| `#C7` | `TSCONF_PORT_OPL3_DATA1` | Bank 1: данные |

**Регистр NEW (Bank 1, reg 0x05):**
- `NEW=1` (0x01) → OPL3 mode: биты L/R в регистрах `C0–C8` управляют маршрутизацией звука
- `NEW=0` (0x00) → OPL2-compat: L+R всегда активны (обязательно для файлов OPL2/OPL1!)

```c
// inc/opl3.h
void opl3_write_b0(uint8_t reg, uint8_t data); // Bank 0 (#C4/#C5)
void opl3_write_b1(uint8_t reg, uint8_t data); // Bank 1 (#C6/#C7)
void opl3_init(void);  // сброс + NEW=1

// Для OPL2/OPL1 VGM: после opl3_init() нужно:
opl3_write_b1(0x05, 0x00);  // NEW=0 — иначе тишина!
```

### Порты AY-3-8910 / YM2149

| Порт    | Константа | Назначение |
|---------|-----------|------------|
| `#FFFD` | `TSCONF_PORT_AY_ADDR` | Регистр адреса / чтение (`OUT/IN`) |
| `#BFFD` | `TSCONF_PORT_AY_DATA` | Запись данных |

**TurboSound (два AY):** второй чип выбирается через порт `#FFFD`:
- `OUT (#FFFD), #FF` (= `TSCONF_AY_TS_CHIP0`) → чип 0 (основной)
- `OUT (#FFFD), #FE` (= `TSCONF_AY_TS_CHIP1`) → чип 1

**Частоты AY** (управление через `wc_turbopl(WC_TURBO_AY, param)`):

| param | Частота | Примечание |
|-------|---------|------------|
| 0 | 1,750,000 Гц | Стандарт TSConfig / Pentagon |
| 1 | 1,773,400 Гц | NTSC-кварц (315/88 МГц ÷ 2) |
| 2 | 3,500,000 Гц | Удвоенная |
| 3 | 3,546,800 Гц | Удвоенная NTSC |

### Порты SAA1099 — MultiSound card

| Чип  | Данные | Адрес | Константы |
|------|--------|-------|-----------|
| SAA1 | `#00FF` | `#01FF` | `TSCONF_PORT_SAA_DATA/ADDR` |
| SAA2 | `#00FE` | `#01FE` | `TSCONF_PORT_SAA2_DATA/ADDR` |

> Некоторые источники указывают SAA2 по портам `#02FF`/`#03FF` — это
> зависит от реализации MultiSound платы. В `tsconf.h` используются
> `#00FE`/`#01FE`.

**Пример записи в регистр OPL3:**
```asm
; Запись в регистр банка 0
    LD BC,#C4      ; Порт адреса
    LD A,reg       ; Номер регистра
    OUT (C),A
    LD B,#C5       ; Порт данных
    LD A,value     ; Значение
    OUT (C),A

; Запись в регистр банка 1
    LD BC,#C6      ; Порт адреса банка 1
    LD A,reg       ; Номер регистра
    OUT (C),A
    LD B,#C7       ; Порт данных банка 1
    LD A,value     ; Значение
    OUT (C),A
```

## Процессор

### Частоты CPU

| Режим | Частота | SysConfig [1:0] | Константа |
|--------|---------|-----------------|----------|
| 00 | 3.5 МГц | `00` | `TSCONF_CPU_3_5MHZ` |
| 01 | 7.0 МГц | `01` | `TSCONF_CPU_7MHZ` |
| 10 | 14.0 МГц | `10` | `TSCONF_CPU_14MHZ` |
| 11 | 28.0 МГц | `11` | (non-standard, не все FPGA) |

> В WC API `wc_turbopl(WC_TURBO_CPU, param)` поддерживает
> param 0–3 (3.5/7/14/28 МГц).

Управление через регистр **SysConfig #20AF** (биты 1:0):

```c
// tsconf.h
#define TSCONF_CPU_3_5MHZ   0x00
#define TSCONF_CPU_7MHZ     0x01
#define TSCONF_CPU_14MHZ    0x02
#define TSCONF_CPU_MASK     0x03
#define TSCONF_CACHE_EN     0x04   // бит 2 — глобальный кэш

// Convenience macros:
tsconf_set_cpu(TSCONF_CPU_14MHZ);                       // только CPU
tsconf_set_cpu_cache(TSCONF_CPU_14MHZ, TSCONF_CACHE_EN); // CPU + кэш
```

### Кэш

512 байт кэш-памяти для ускорения работы на 14/28 МГц.

**Глобально:** бит 2 регистра **SysConfig** (`TSCONF_CACHE_EN`).

**По-оконно:** регистр **CacheConfig #2BAF**:

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TSCONF_CACHE_EN_0000` | 0x01 | Кэш для #0000–#3FFF |
| `TSCONF_CACHE_EN_4000` | 0x02 | Кэш для #4000–#7FFF |
| `TSCONF_CACHE_EN_8000` | 0x04 | Кэш для #8000–#BFFF |
| `TSCONF_CACHE_EN_C000` | 0x08 | Кэш для #C000–#FFFF |
| `TSCONF_CACHE_ALL` | 0x0F | Все окна |
| `TSCONF_CACHE_NONE` | 0x00 | Кэш выключен |

```c
tsconf_set_cache(TSCONF_CACHE_EN_8000 | TSCONF_CACHE_EN_C000);
```

**Важно:** После DMA операций необходима **инвалидация кэша**:

```asm
; Инвалидация: 512 байт LDIR на себя
    LD HL,#FE00
    LD DE,#FE00
    LD BC,#0200
    LDIR
```

## Прерывания

### Источники прерываний

TSConfig поддерживает несколько источников INT:

| Источник | Бит | IM2 вектор | Константа |
|----------|-----|------------|-----------|
| FRAME | 0 | `#FF` (`TSCONF_IVEC_FRAME`) | `TSCONF_INT_FRAME` |
| LINE | 1 | `#FD` (`TSCONF_IVEC_LINE`) | `TSCONF_INT_LINE` |
| DMA | 2 | `#FB` (`TSCONF_IVEC_DMA`) | `TSCONF_INT_DMA` |
| WTP | 3 | — | `TSCONF_INT_WTP` |

### Регистр маски прерываний (INTMask #2AAF)

```
Биты:
3 - WTP (Wait Port) interrupt enable
2 - DMA interrupt enable
1 - LINE interrupt enable
0 - FRAME interrupt enable

Reset: #01 (только FRAME включен)
```

```c
tsconf_set_intmask(TSCONF_INT_FRAME | TSCONF_INT_LINE);
```

### Позиционирование FRAME-INT

```c
// Задать вертикальную строку и горизонтальную позицию
tsconf_set_frame_int(vline, hpos);
// Отключить
tsconf_disable_frame_int();

// Авто-инкремент VCNT (для множественных INT в кадре):
// VSINTH[7:4] = auto-increment value
#define TSCONF_VSINTH_INC(n)    ((n) << 4)
```

---

## MemConfig (#21AF) — Конфигурация памяти

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TSCONF_MEM_ROM128` | 0x01 | Выбор банка ROM (= бит 4 порта #7FFD) |
| `TSCONF_MEM_W0_WE` | 0x02 | Разрешить запись в окно #0000 |
| `TSCONF_MEM_W0_MAP_DIS` | 0x04 | Отключить маппинг через Page0 |
| `TSCONF_MEM_W0_RAM` | 0x08 | RAM вместо ROM в окне #0000 |
| `TSCONF_MEM_LCK128_512K` | 0x00 | Pentagon-512 режим |
| `TSCONF_MEM_LCK128_128K` | 0x40 | Pentagon-128 режим |
| `TSCONF_MEM_LCK128_AUTO` | 0x80 | Авто-определение |
| `TSCONF_MEM_LCK128_1M` | 0xC0 | Расширенный 1 МБ |

---

## TSU — Tile/Sprite Engine (#06AF)

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TSCONF_TS_EXT` | 0x01 | TSU 360×288 (extended) |
| `TSCONF_T0Z_EN` | 0x04 | Tile[0] visible (plane 0) |
| `TSCONF_T1Z_EN` | 0x08 | Tile[0] visible (plane 1) |
| `TSCONF_T0_EN` | 0x20 | Tile layer 0 enable |
| `TSCONF_T1_EN` | 0x40 | Tile layer 1 enable |
| `TSCONF_S_EN` | 0x80 | Sprite layers enable |

---

## HUS — Hurricane Sound Engine

> Порты `#80AF`–`#87AF` (есть не на всех FPGA-прошивках)

HUS — аппаратный звуковой движок TSConfig для воспроизведения
PCM-сэмплов из RAM с DMA.

| Порт | Константа | Описание |
|------|-----------|----------|
| `#80AF` | `TSCONF_PORT_HUSCTRL` | Control (W) / Status (R) |
| `#81AF` | `TSCONF_PORT_HUSSAMPRATE` | Sample Rate: 1,750,000 / (SR+1) |
| `#82AF` | `TSCONF_PORT_HUSTICKRATEL` | Tick Rate (low) |
| `#83AF` | `TSCONF_PORT_HUSTICKRATEH` | Tick Rate (high) |
| `#84AF`–`#87AF` | `TSCONF_PORT_HUSRLD0`–`3` | Reload ch 0–31 |

**Биты статуса (чтение #80AF):**

| Константа | Бит | Описание |
|-----------|-----|----------|
| `TSCONF_HUS_ACT` | 0 | 1 = HUS активен |
| `TSCONF_HUS_TICK` | 1 | 1 = tick event |
| `TSCONF_HUS_HALF` | 6 | 1 = полбуфера |
| `TSCONF_HUS_EMPTY` | 7 | 1 = буфер пуст |

**Управление (запись #80AF):**

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TSCONF_HUS_PAUSE` | 0x02 | Пауза воспроизведения |
| `TSCONF_HUS_RESET` | 0x80 | Сброс HUS |

### ISR плагина — собственный таймер

Плагин может зарегистрировать собственный ISR через
`wc_int_pl(WC_INT_PLUGIN)` + `wc_int_pl_handler(addr)`.

В VGM-плеере ISR настраивается через **перехват** вектора IM2
(FRAME INT, вектор `#5BFF`) с высокой частотой вызова.
POS_TABLE задаёт позиции INT-триггеров в кадре:

| Вариант | ISR_FREQ | T-states/тик | Тиков/кадр | VGM_SAMPLE_SHIFT | Семплов/тик |
|---------|----------|-------------|------------|------------------|---------|
| Quarter | **683 Гц** | 5120 T | 14 | 6 (44100/64) | 64 |
| Half | 1367 Гц | 2560 T | 28 | 5 (44100/32) | 32 |
| Full | 2734 Гц | 1280 T | 56 | 4 (44100/16) | 16 |

Конфигурация варианта хранится в `inc/variant_cfg.h`:

```c
// variant_cfg.h (текущий вариант)
#define ISR_FREQ             683
#define ISR_TICKS_PER_FRAME  14      // 683/50≈14
#define VGM_SAMPLE_SHIFT     6       // 44100 >> 6 = 689 ≈ 683
#define VGM_SAMPLE_MASK      0x3Fu
#define VGM_FILL_CMD_BUDGET  32      // команд за один yield
#define VARIANT_POS_ENTRIES  14      // записей в pos_table
#define VARIANT_POS_STEP     5120    // T-states между INT
```

### Механизм pos_table

`pos_table` — кольцевой буфер позиций INT в кадре.
Каждая запись: 5 байт `[VSINTH, VSINTL, HSINT, next_ptr_lo, next_ptr_hi]`.
ISR выводит 3 байта через OUTI (порты VSINTH/VSINTL/HSINT),
затем переходит по next_ptr к следующей записи.
Генерируется скриптом `scripts/gen_pos_table.js`.

### Частота прерываний TSConfig (FRAME)

- FRAME: зависит от видео режима
  - 50 Гц (312 строк PAL)
  - 60 Гц (262 строки NTSC)
  - ~48.8 Гц (320 строк Pentagon)

## DMA Контроллер

TSConfig имеет аппаратный DMA для быстрого копирования/заполнения данных.

### Режимы DMA (регистр DMACTRL #27AF)

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TSCONF_DMA_RAM_RAM` | 0x09 | RAM → RAM копирование |
| `TSCONF_DMA_BLT1_RAM` | 0x89 | RAM → RAM с пропуском нулей (transparency) |
| `TSCONF_DMA_BLT2_RAM` | 0x31 | Blitter с aдером |
| `TSCONF_DMA_FILL` | 0x21 | Заполнение RAM |
| `TSCONF_DMA_RAM_CRAM` | 0xA1 | RAM → CRAM (палитра, RGB555) |
| `TSCONF_DMA_RAM_SFILE` | — | RAM → Sprite File |
| `TSCONF_DMA_SPI_RAM` | 0x11 | SPI → RAM (чтение SD) |
| `TSCONF_DMA_RAM_SPI` | 0x91 | RAM → SPI (запись SD) |
| `TSCONF_DMA_IDE_RAM` | 0x19 | IDE → RAM |
| `TSCONF_DMA_RAM_IDE` | 0x99 | RAM → IDE |
| `TSCONF_DMA_FDD_RAM` | 0x29 | FDD → RAM |
| `TSCONF_DMA_RAM_WTP` | 0xB9 | RAM → Wait Port |
| `TSCONF_DMA_WTP_RAM` | 0x39 | Wait Port → RAM |

### Флаги DMA

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TSCONF_DMA_ASZ_256` | 0x00 | Выравнивание 256 байт |
| `TSCONF_DMA_ASZ_512` | 0x04 | Выравнивание 512 байт |
| `TSCONF_DMA_D_ALGN` | 0x08 | Выравнивание приёмника |
| `TSCONF_DMA_S_ALGN` | 0x10 | Выравнивание источника |
| `TSCONF_DMA_ACT` | 0x80 | DMA active (бит статуса при чтении) |

### Wait Port Device (#25AF)

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TSCONF_DMAWP_GLUCLOCK` | 0x00 | GluClock |
| `TSCONF_DMAWP_COMPORT` | 0x01 | COM-порт |

### Основные регистры DMA

> Порты DMA описаны выше в таблице портов. Основные:

| Регистр | Порт | Описание |
|---------|------|----------|
| DMA_SAL/H/X | `#19AF`/`#1AAF`/`#1BAF` | Адрес + страница источника |
| DMA_DAL/H/X | `#1CAF`/`#1DAF`/`#1EAF` | Адрес + страница приёмника |
| DMA_LEN | `#26AF` | Размер бёрста (N) |
| DMA_NUM/H | `#28AF`/`#2CAF` | Количество бёрстов (T) |
| DMA_CTRL | `#27AF` | Режим + запуск (W) / статус (R) |

### Пример DMA через WC API (C)

```c
// Копирование 4 КБ из стр. 0 плагина (#8000) в стр. 1 (#8000)
wc_dmapl(WC_DMA_INIT_SD);  // потребует B,C,HL,DE в регистрах
wc_dmapl(WC_DMA_SET_TN);   // B=бёрсты, C=размер бёрста
wc_dmapl(WC_DMA_RUN_RAM);  // запуск RAM→RAM с ожиданием
```

---

## Использование портов из SDCC

`tsconf.h` объявляет все порты как `__sfr __banked` переменные для
прямого ввода/вывода без `__asm`:

```c
// Примеры:
sfr_page2 = 5;                    // OUT (#12AF), 5
sfr_sysconfig = TSCONF_CPU_14MHZ; // OUT (#20AF), 2
sfr_border = 2;                   // OUT (#0FAF), 2
sfr_opl3_addr0 = 0x20;            // OUT (#00C4), 0x20
sfr_opl3_data0 = 0xFF;            // OUT (#00C5), 0xFF

// Чтение:
uint8_t page = sfr_page2;         // IN A,(#12AF)
```

Полный список: `sfr_vconfig`, `sfr_vpage`, `sfr_gxoffsl/h`, `sfr_gyoffsl/h`,
`sfr_tsconfig`, `sfr_palsel`, `sfr_border`, `sfr_page0`–`sfr_page3`,
`sfr_sysconfig`, `sfr_memconfig`, `sfr_intmask`, `sfr_cacheconfig`,
`sfr_dmasaddrh/l/x`, `sfr_dmadaddrh/l/x`, `sfr_dmalen`, `sfr_dmactrl`,
`sfr_dmanum/h`, `sfr_opl3_addr0/data0/addr1/data1`, `sfr_ay_addr/data`,
`sfr_saa_addr/data`, `sfr_saa2_addr/data`.

## Видео система

### Видео режимы (VConfig #00AF)

| Режим | Константа | Разрешения | Цвета |
|-------|-----------|------------|-------|
| ZX | `TSCONF_VM_ZX` (0x00) | 256×192 | 16 (атрибуты) |
| 16c | `TSCONF_VM_16C` (0x01) | 256..360 | 16 (4 bpp) |
| 256c | `TSCONF_VM_256C` (0x02) | 256..360 | 256 (8 bpp) |
| TXT | `TSCONF_VM_TXT` (0x03) | 80×25, 80×30, 90×36 | 256 |

**Разрешения:**

| Константа | Значение | Пиксели |
|-----------|----------|---------|
| `TSCONF_RRES_256x192` | 0x00 | 256×192 |
| `TSCONF_RRES_320x200` | 0x40 | 320×200 |
| `TSCONF_RRES_320x240` | 0x80 | 320×240 |
| `TSCONF_RRES_360x288` | 0xC0 | 360×288 |

**Прочие биты VConfig:**

| Константа | Бит | Описание |
|-----------|-----|----------|
| `TSCONF_VC_FT_EN` | 2 | FT812 enable (VDAC2) |
| `TSCONF_VC_GFXOVR` | 3 | GFX over TSU |
| `TSCONF_VC_NOTSU` | 4 | Отключить TSU graphics |
| `TSCONF_VC_NOGFX` | 5 | Отключить main graphics |

**Чтение VConfig (статус):**

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TSCONF_VDVER_2BIT_PWM` | 0x00 | 2-бит ЦАП (ШИМ) |
| `TSCONF_VDVER_3BIT` | 0x01 | 3-бит |
| `TSCONF_VDVER_4BIT` | 0x02 | 4-бит |
| `TSCONF_VDVER_5BIT` | 0x03 | 5-бит |
| `TSCONF_VDVER_VDAC2` | 0x07 | VDAC2 / FT812 |
| `TSCONF_STATUS_PWR_UP` | 0x40 | Після включення |

### Палитра

256 ячеек палитры, формат RGB555 (15 бит цвета).

### Спрайты

- До 85 спрайтов на строку
- Размеры: 8x8 до 64x64 пикселей
- До 3 плоскостей спрайтов
- До 16 палитр на строку

## Совместимость с Pentagon-128

### Порт #7FFD

```
Биты:
7:6 - дополнительные биты страницы (512K/1024K)
5   - LOCK (блокировка переключения)
4   - ROM128 (выбор ROM)
3   - SCR (экран 5 или 7)
2:0 - номер страницы для #C000
```

### Режимы совместимости (MemConfig [7:6])

| LCK128 | Режим | Описание |
|--------|-------|----------|
| 00 | Pentagon-512 | `TSCONF_MEM_LCK128_512K` |
| 01 | Pentagon-128 | `TSCONF_MEM_LCK128_128K` |
| 10 | Авто | `TSCONF_MEM_LCK128_AUTO` |
| 11 | 1024 КБ | `TSCONF_MEM_LCK128_1M` |

---

## FDD виртуализация (#29AF)

TSConfig позволяет виртуализировать FDD-приводы (для TRD/TAP):

| Константа | Значение | Описание |
|-----------|----------|----------|
| `TSCONF_FDD_A` | 0x01 | Привод A |
| `TSCONF_FDD_B` | 0x02 | Привод B |
| `TSCONF_FDD_C` | 0x04 | Привод C |
| `TSCONF_FDD_D` | 0x08 | Привод D |
| `TSCONF_FDD_VG_OPEN` | 0x80 | Открыть VG93 |

---

## Файловая система

WC работает через FAT32:
- **SD-карты**: Z-Controller (основной), NeoGS SD
- **IDE**: NEMO (Master/Slave), SMUC (Master/Slave)
- **Длинные имена**: до 255 символов (с WC v0.73)
- **Каталоги**: >4095 записей (с WC v1.05)

---

## Полезные ссылки

- TSConfig спецификация: https://github.com/tslabs/zx-evo
- Регистры TSConfig: [src_sdcc/inc/tsconf.h](../src_sdcc/inc/tsconf.h)
- WC API: [src_sdcc/inc/wc_api.h](../src_sdcc/inc/wc_api.h)
- ISR конфигурация: [src_sdcc/inc/variant_cfg.h](../src_sdcc/inc/variant_cfg.h)

## Пример инициализации для плагина

```asm
; Базовая инициализация для музыкального плагина

INIT_PLUGIN:
    ; Сохранить текущее состояние
    LD A,(#6002)      ; Страница в окне 2
    PUSH AF
    LD A,(#6003)      ; Страница в окне 3
    PUSH AF

    ; Восстановить TXT режим
    LD A,#0F
    CALL WLD          ; GEDPL
    
    ; Установить частоту CPU
    LD B,#00          ; Z80 frequency
    LD C,%01          ; 7 MHz
    LD A,#0E
    CALL WLD          ; TURBOPL
    
    ; Настроить прерывания
    LD HL,INT_HANDLER
    EXA
    LD A,#FF
    CALL WLD          ; INT_PL
    EXA
    
    ; Инициализация OPL3
    CALL INIT_OPL3
    
    RET

; Обработчик прерываний
INT_HANDLER:
    PUSH AF, BC, DE, HL
    ; Ваш код обработки музыки
    CALL UPDATE_MUSIC
    POP HL, DE, BC, AF
    EI
    RETI

; Инициализация OPL3
INIT_OPL3:
    ; Сброс OPL3
    LD BC,#C4
    XOR A
    OUT (C),A
    LD B,#C5
    OUT (C),A
    ; Дополнительная инициализация...
    RET
```

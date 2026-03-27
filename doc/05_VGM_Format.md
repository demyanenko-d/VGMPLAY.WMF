# VGM Format Reference

> **Источник истины:** `src_sdcc/inc/vgm.h`, `src_sdcc/lib/vgm.c`

## Обзор формата VGM

VGM (Video Game Music) — формат хранения логов регистров звуковых чипов.

### Основные характеристики

- Расширение файла: **.vgm** (raw) или **.vgz** (gzip-сжатый)
- Содержит команды записи в регистры звуковых чипов
- Поддерживаемые плагином чипы: **YMF262 (OPL3), YM3812 (OPL2), YM3526 (OPL1), Y8950, AY-3-8910/YM2149/YM2203, SAA1099**
- Частота семплирования фиксирована: **44100 Гц**

## Структура заголовка VGM (ключевые смещения)

> Все поля — little-endian 32 бит, если не указано иное.  
> Минимальная поддерживаемая версия: **1.00** (`0x00000100`).

| Offset | Размер | Название         | Описание                                       | С версии |
|--------|--------|------------------|------------------------------------------------|----------|
| `0x00` | 4      | Ident            | Сигнатура `"Vgm "` (0x56 0x67 0x6D 0x20)      | 1.00     |
| `0x04` | 4      | EoF offset       | Расстояние до EOF от позиции 0x04              | 1.00     |
| `0x08` | 4      | Version          | BCD: `0x0150`=v1.50, `0x0151`=v1.51           | 1.00     |
| `0x0C` | 4      | SN76489 clock    | Тактовая частота SN76489 (0 = нет)             | 1.00     |
| `0x10` | 4      | YM2413 clock     | Тактовая частота YM2413 / OPLL (0 = нет)      | 1.00     |
| `0x14` | 4      | GD3 offset       | Смещение GD3-тега от `0x14` (0 = нет тега)    | 1.00     |
| `0x18` | 4      | Total samples    | Полная длина трека в семплах (÷44100 = сек)   | 1.00     |
| `0x1C` | 4      | Loop offset      | Смещение точки петли от `0x1C` (0 = нет)      | 1.00     |
| `0x20` | 4      | Loop samples     | Длина тела петли в семплах                     | 1.00     |
| `0x24` | 4      | Rate             | Кадровая частота (50/60 Гц, информативно)      | 1.01     |
| `0x34` | 4      | **VGM data offset** | Смещение VGM-данных от `0x34`; 0 → данные с `0x40` | **1.50** |
| `0x44` | 4      | YM2203 clock     | **YM2203 (OPN/PSG)** (0 = нет)                | 1.51     |
| `0x50` | 4      | **YM3812 clock** | **YM3812 (OPL2)** — AdLib/SoundBlaster        | **1.51** |
| `0x54` | 4      | **YM3526 clock** | **YM3526 (OPL1)** — ISA AdLib                 | **1.51** |
| `0x58` | 4      | **Y8950 clock**  | **Y8950 (OPL+ADPCM)** — MSX-Audio             | **1.51** |
| `0x5C` | 4      | **YMF262 clock** | **YMF262 (OPL3)** — SB Pro 2 / TSConfig       | **1.51** |
| `0x60` | 4      | YMF278B clock    | YMF278B (OPL4) (0 = нет)                      | 1.51     |
| `0x74` | 4      | AY8910 clock     | **AY-3-8910 / YM2149** (0 = нет)              | 1.51     |
| `0xC8` | 4      | SAA1099 clock    | **SAA1099** — SAM Coupé (0 = нет)             | 1.71     |

> ⚠️ **Частая ошибка в документации:** смещения чипов в диапазоне `0x50`–`0x5C` — не `0x5C`/`0x60`/`0x64`/`0x68`.  
> Используйте константы: `VGM_OFF_YM3812=0x50`, `VGM_OFF_YM3526=0x54`, `VGM_OFF_Y8950=0x58`, `VGM_OFF_YMF262=0x5C`.

### Смещение данных VGM

```
Если заголовок[0x34] (VGM data offset) == 0:
    данные начинаются с абсолютного адреса 0x40  (старые файлы, < v1.50)
Иначе:
    данные начинаются с 0x34 + заголовок[0x34]
```

Типичное значение для v1.51+: `VGM_data_offset = 0x4C`, данные с `0x80`.  
Реализация в `vgm_parse_header()`: хранит готовый адрес начала данных в `vgm_data_start`.

### Bit 30 поля clock — флаг dual-chip

Бит 30 любого clock-поля (`clock & 0x40000000`) означает **два экземпляра** чипа (`VGM_CF_DUAL`).  
Реальная тактовая частота = `clock & 0x3FFFFFFF`.

### Обнаружение чипов — `vgm_parse_header()`

Приоритет определения `vgm_chip_type`: **YMF262 > YM3812 > YM3526/Y8950**:

```c
// vgm.h — константы
#define VGM_OFF_YM3812   0x50   // OPL2
#define VGM_OFF_YM3526   0x54   // OPL1
#define VGM_OFF_Y8950    0x58   // OPL+ADPCM
#define VGM_OFF_YMF262   0x5C   // OPL3
#define VGM_OFF_AY8910   0x74   // AY-3-8910 / YM2149
#define VGM_OFF_YM2203   0x44   // OPN (PSG-часть = AY)
#define VGM_OFF_SAA1099  0xC8   // SAA1099

// Значения vgm_chip_type
#define VGM_CHIP_NONE    0
#define VGM_CHIP_OPL     1   // YM3526 или Y8950
#define VGM_CHIP_OPL2    2   // YM3812
#define VGM_CHIP_OPL3    3   // YMF262
```

## VGM-команды — полная таблица

| Код          | Байт | Параметры | Описание                                      |
|--------------|------|-----------|-----------------------------------------------|
| `0x55`       | 3    | `rr dd`   | YM2203 write (PSG-часть, reg 0–15)            |
| `0x5A`       | 3    | `rr dd`   | YM3812 (OPL2) write                           |
| `0x5B`       | 3    | `rr dd`   | YM3526 (OPL1) write                           |
| `0x5C`       | 3    | `rr dd`   | Y8950 (OPL+ADPCM) write                      |
| `0x5E`       | 3    | `rr dd`   | YMF262 (OPL3) Bank 0 write                   |
| `0x5F`       | 3    | `rr dd`   | YMF262 (OPL3) Bank 1 write                   |
| `0x61`       | 3    | `lo hi`   | Задержка: `(hi<<8)\|lo` семплов              |
| `0x62`       | 1    | —         | Задержка 735 семплов (1/60 с)                |
| `0x63`       | 1    | —         | Задержка 882 семпла (1/50 с)                 |
| `0x66`       | 1    | —         | End of data → конец потока / loop             |
| `0x67`       | var  | —         | Data block (PCM и пр.) — пропускается         |
| `0x70`–`0x7F`| 1    | —         | Задержка 1–16 семплов (`(cmd & 0x0F) + 1`)   |
| `0xA0`       | 3    | `rr dd`   | AY8910 write; бит 7 `rr` = чип 2             |
| `0xBD`       | 3    | `rr dd`   | SAA1099 write; бит 7 `rr` = чип 2            |

Прочие команды пропускаются по таблице длин VGM spec (размер 1, 2, или 3 байта).

---

## Масштабирование задержек

VGM sample rate = **44100 Гц**, ISR rate = **1367 Гц** (= 44100 / 32):

```c
// inc/isr.h
#define VGM_SAMPLE_SHIFT  5      // делитель = 2^5 = 32
#define VGM_SAMPLE_MASK   0x1Fu  // маска остатка
```

Алгоритм `vgm_fill_buffer()`:
- Накапливает семплы задержек
- При накоплении ≥ 32 → конвертирует `samples >> 5` в ISR-тики → команда `CMD_WAIT`
- Задержки < 32 семплов (< 0.73 мс) **теряются** (rounded down, это нормально)

```
1 секунда      = 44100 семплов = 1378 ISR-тиков ≈ 1367 Гц
1 фрейм (50Hz) =   882 семпла  =   27 ISR-тиков
1 фрейм (60Hz) =   735 семплов =   22 ISR-тика
```

---

## GD3 — метаданные трека

GD3-тег содержит Unicode-строки (UCS-2 LE, нуль-терминированные):

| № | Содержимое         |
|---|--------------------|
| 1 | Track title (EN)   |
| 2 | Track title (JP)   |
| 3 | Game name (EN)     |
| 4 | Game name (JP)     |
| 5 | System name (EN)   |
| 6 | System name (JP)   |
| 7 | Author (EN)        |
| 8 | Author (JP)        |
| 9 | Release date       |
|10 | VGM pack author    |
|11 | Notes              |

В плагине читаются только **английские** поля (нечётные), конвертируются в ASCII: символы > 127 → `'?'`.  
Макс. длина поля: `VGM_GD3_LEN = 48` байт.

```c
// inc/vgm.h
extern char vgm_gd3_track[VGM_GD3_LEN];
extern char vgm_gd3_game[VGM_GD3_LEN];
extern char vgm_gd3_system[VGM_GD3_LEN];
extern char vgm_gd3_author[VGM_GD3_LEN];
```

---

## Петля (loop)

```c
// inc/vgm.h
extern uint16_t vgm_loop_addr;  // адрес точки петли в окне #C000 (0 = нет)
extern uint8_t  vgm_loop_page;  // VPL-страница точки петли
```

Если `loop_offset (0x1C) != 0`:
- Абсолютное смещение в файле = `0x1C + loop_offset`
- `vgm_rewind_to_loop()` переставляет `vgm_read_ptr` / `vgm_cur_page` на точку петли

---

## VGZ — gzip-сжатый VGM

Файл `.vgz` — стандартный gzip. Схема обработки в плагине:

```
1. Загрузить .vgz в мегабуфер (VPL-страницы выше #20)
2. copy_pages_to_tap(n)  — скопировать во временную TAP-область (#A1–#BF, 31 стр.)
3. inflate_vgz(src, dst) — распаковать (inflate.asm) обратно в мегабуфер
4. Работать с VGM как обычно
```

Макс. размер распакованного VGM через TAP: 31 × 16 КБ = **496 КБ**.

```c
void copy_pages_to_tap(uint8_t n_pages);
uint8_t inflate_vgz(uint8_t src_page, uint8_t dst_page);
// возвращает: 0 = успех, 1 = ошибка (не gzip или CRC-ошибка)
```

---

## OPL2 vs OPL3 — проблема L/R output

**Проблема:** `opl3_init()` устанавливает `NEW=1` (OPL3 mode).  
В OPL3 mode биты 4–5 регистров `C0–C8` управляют маршрутизацией L/R (`L=1, R=1` для звука).  
Файлы OPL2/OPL1 (команды `0x5A`/`0x5B`) пишут `C0` без битов L/R → **тишина**.

**Решение в `start_playback()` (main.c):**

```c
opl3_init();                           // NEW=1 по умолчанию
if (vgm_chip_type != VGM_CHIP_OPL3) {
    opl3_write_b1(0x05, 0x00);         // NEW=0: OPL2-совместимый режим
}
// В OPL2-compat звук всегда идёт на оба канала L+R
```

---

## Пример парсинга заголовка (SDCC C)

```c
#include "inc/vgm.h"
#include "inc/wc_api.h"

// Загрузить первую страницу файла в окно #C000 и разобрать заголовок
wc_mngcvpl(0);           // Переключить Win3 (#C000) на VPL-страницу 0
wc_load512(0xC000, 32);  // Загрузить 16 КБ с диска

uint8_t rc = vgm_parse_header();

switch (rc) {
case VGM_OK:
    // vgm_chip_type  → VGM_CHIP_OPL / OPL2 / OPL3
    // vgm_chip_count → кол-во обнаруженных чипов
    // vgm_version    → 0x0151 для v1.51
    // vgm_loop_addr  → 0 = нет петли
    break;
case VGM_ERR_HEADER:
    // Нет сигнатуры "Vgm " или заголовок повреждён
    break;
case VGM_ERR_NOOPL:
    // Файл не содержит поддерживаемого чипа (OPL/AY/SAA)
    break;
}
```

---

## Полезные константы (из `vgm.h`)

```c
// Смещения clock-полей в заголовке (relative byte offsets)
#define VGM_OFF_SN76489  0x0C
#define VGM_OFF_YM2413   0x10
#define VGM_OFF_YM2203   0x44   // OPN
#define VGM_OFF_YM3812   0x50   // OPL2   ← основной
#define VGM_OFF_YM3526   0x54   // OPL1
#define VGM_OFF_Y8950    0x58   // MSX-Audio
#define VGM_OFF_YMF262   0x5C   // OPL3   ← основной
#define VGM_OFF_YMF278B  0x60   // OPL4
#define VGM_OFF_AY8910   0x74   // AY-3-8910 / YM2149
#define VGM_OFF_SAA1099  0xC8   // SAA1099

// Коды результата vgm_parse_header()
#define VGM_OK           0
#define VGM_ERR_HEADER   1
#define VGM_ERR_NOOPL    2

// ISR / VGM sample rate mapping
#define VGM_SAMPLE_SHIFT 5   // 44100 / 32 ≈ 1367 Гц (ISR_FREQ)
```

---

## Ссылки

- VGM Format Specification: https://vgmrips.net/wiki/VGM_Specification
- YMF262 (OPL3) Datasheet: Yamaha YMF262 Application Manual
- Архив VGM-треков: https://vgmrips.net/

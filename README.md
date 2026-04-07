# VGMPLAY.WMF — VGM Player for TSConfig / Wild Commander

VGM/VGZ player plugin for **Wild Commander** (WC) on **ZX Spectrum TSConfig** with
**MultiSound FPGA** sound card.  Written in C (SDCC) + Z80 assembly.

**Version:** v0.9-beta

## Supported Sound Chips

| Chip | VGM cmd | Notes |
|------|---------|-------|
| **OPL1** (YM3526) | `0x5A`/`0x5B` | Через MultiSound OPL3 (compat mode) |
| **OPL2** (YM3812) | `0x5A`/`0x5B` | Через MultiSound OPL3 (compat mode) |
| **OPL3** (YMF262) | `0x5E`/`0x5F` | NEW=1, стерео L/R через C0-C8 |
| **YMF278B** (OPL4 FM) | `0xD0` | FM-часть через OPL3 |
| **AY-3-8910 / YM2149** | `0xA0` | Single + dual chip |Та
| **YM2203** (OPN) | `0x55`/`0xA0` | SSG-часть через AY |
| **SAA1099** | `0xBD` | Single + dual chip (bit7 chip select) |

**Форматы:** VGM (raw), VGZ (gzip — распаковка через inline inflate).

## Features

- **GD3 metadata** — отображение названия трека, игры, автора, системы
- **Двойная буферизация** — ISR 683 Hz проигрывает команды, main loop наполняет буферы
- **Спектроанализатор** — 16-полосный в реальном времени с плавным затуханием
- **Progress bar** — текущее время / общая длительность + счётчик loop
- **VGZ inflate** — распаковка gzip прямо в память с progress bar
- **Dual chip** — поддержка двухчиповых VGM (AY×2, SAA×2, YM2203×2)
- **SAA chip select** — переключение chip0/chip1 клавишами 1/2
- **Cmdblocks** — предрассчитанные блоки инициализации/silence в SRAM
- **PS/2 клавиатура** — прямой доступ через CMOS FIFO TSConfig (без WC API)
- **Варианты сборки** — build_variants.ps1 генерирует множество WMF с разными ISR freq / budget

## Управление (PS/2 клавиатура)

| Клавиша | Действие |
|---------|----------|
| **Space** | Следующий трек |
| **P** | Предыдущий трек |
| **Esc** | Выход из плагина |
| **1** | SAA режим 1 — chip0 (левый SAA1099) |
| **2** | SAA режим 2 — chip1 (правый SAA1099) |

> Клавиши 1/2 активны только для dual-SAA музыки на одночиповой плате (`saa_chips=1`).
> На двухчиповой плате (`saa_chips=2`) режим выбирается автоматически.

## Настройки (wc.ini)

Параметры задаются в конфигурации WC:

```ini
VGMPLAY.WMF -min_dur=10 -max_dur=4 -loops=1 -saa=1 -saa_chips=1
```

| # | Параметр | Default | Описание |
|---|----------|---------|----------|
| 0 | `min_dur` | 10 | Минимальная длительность трека (секунды) |
| 1 | `max_dur` | 4 | Максимальная длительность (минуты) |
| 2 | `loops` | 1 | Количество проигрываний loop-секции |
| 3 | `saa` | 1 | SAA режим: **1** = chip0, **2** = chip1 |
| 4 | `saa_chips` | 1 | Конфигурация платы: **1** = один SAA (default), **2** = два SAA |

### Логика SAA режимов

- **`saa_chips=1`** (одночиповая плата, default):
  - Single-SAA музыка → всегда играет на chip0
  - Dual-SAA музыка → по умолчанию chip0; переключение клавишами 1/2
- **`saa_chips=2`** (двухчиповая плата):
  - Single-SAA музыка → только chip0
  - Dual-SAA музыка → turbo mode (оба чипа одновременно), переключение недоступно

## Сборка

### Требования

- **SDCC** — компилятор C для Z80 (sdcc, sdasz80, sdld)
- **sjasmplus** — ассемблер для cmdblocks и inflate
- **Node.js** — скрипты генерации (pos_table, freq_tables, ihx2wmf)

### Основная сборка

```bat
cd src_sdcc
build.bat
```

Результат: `VGMPLAY.WMF` в корне проекта + `tsconf.img` (образ диска).

### Варианты сборки

```powershell
cd src_sdcc
.\build_variants.ps1
```

Генерирует все комбинации ISR freq / budget / no-short-waits в папку `variants/`.

Параметры варианта задаёт единственный файл `inc/variant_cfg.h`:

| Параметр | Описание |
|----------|----------|
| `ISR_FREQ` | Частота ISR (683 / 1367 / 2734 Hz) |
| `VGM_FILL_CMD_BUDGET` | Макс. VGM-команд между yield (8..64) |
| `VARIANT_DATA_LOC` | Адрес начала сегмента _DATA (hex) |
| `VGM_NO_SHORT_WAITS` | Отключить обработку пауз 0x70-0x7F |

## Структура проекта

```
src_sdcc/           Исходный код (C + ASM)
  main.c            Главный модуль: UI, playback loop, INI
  lib/vgm.c         VGM парсер, fill_buffer, emit handlers
  lib/keys.c        PS/2 клавиатура (FSM, CMOS FIFO)
  asm/isr.s         ISR: двойная буферизация, pos_table
  asm/spectrum.s    Спектроанализатор (рендер + decay)
  asm/cmdblocks.asm Блоки init/silence для всех чипов
  asm/inflate_call.s Обёртка inflate для VGZ
  inc/              Заголовочные файлы
  scripts/          Node.js утилиты сборки
  build/            Временные артефакты
variants/           Предсобранные WMF для A/B тестирования
DiskRef/            Эталонные файлы диска, тестовая музыка
Ref/                Документация чипов, SDK, FPGA reference
doc/                Описание WC Plugin API, VGM формата
SamFlate/           Z80 inflate (распаковка gzip)
tools/              sjasm, Unreal emulator
```

## Аппаратные требования

- **ZX Spectrum TSConfig** (ZX Evolution с TSConf FPGA)
- **Wild Commander** (ОС/файловый менеджер)
- **MultiSound FPGA** — звуковая карта (OPL3 + AY + SAA1099)
- CPU 14 MHz (turbo mode, автоматически включается плагином)
- PS/2 клавиатура

## Лицензия

Проект предназначен для использования с аппаратной платформой TSConfig.

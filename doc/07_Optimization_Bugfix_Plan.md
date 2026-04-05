# VGM Player — План оптимизаций и исправлений

Документ составлен по результатам полного анализа кодовой базы.
Приоритет: критичные баги → звуковые баги → производительность → UI → косметика.

---

## Содержание

1. [BUG: Рассинхронизация decay-таймеров (spectrum.s vs variant_cfg.h)](#1-bug-рассинхронизация-decay-таймеров)
2. [BUG: SAA1099 — нет звука](#2-bug-saa1099--нет-звука)
3. [BUG: YM2203 — тон выше чем надо (≈2×?)](#3-bug-ym2203--тон-выше-чем-надо)
4. [Анализ: пропуск нот / race conditions](#4-анализ-пропуск-нот--race-conditions)
5. [Оптимизация: spectrum_render — skip пустых баров](#5-оптимизация-spectrum_render--skip-пустых-баров)
6. [Оптимизация: spectrum_decay — early-exit на all-zero](#6-оптимизация-spectrum_decay--early-exit)
7. [Оптимизация: emit_wait — table-based shift](#7-оптимизация-emit_wait--table-based-shift)
8. [UI: наложение метаданных при chip_count > 2](#8-ui-наложение-метаданных-при-chip_count--2)
9. [UI: центровка окна по текстовому режиму WC](#9-ui-центровка-окна-по-текстовому-режиму-wc)
10. [Debug: border-flash under #ifdef](#10-debug-border-flash-under-ifdef)

---

## 1. BUG: Рассинхронизация decay-таймеров

**Приоритет: ВЫСОКИЙ**  
**Файлы:** `asm/spectrum.s`, `inc/variant_cfg.h`

### Проблема

Константы decay-таймеров в ASM **не совпадают** с определениями в конфиг-файле:

| Константа | `variant_cfg.h` | `spectrum.s` (EQU) | Расхождение |
|-----------|------------------|--------------------|-------------|
| DECAY_FAST | 2 | 1 | ASM в 2× быстрее |
| DECAY_MED | 3 | 2 | ASM в 1.5× быстрее |
| DECAY_SLOW | 5 | 4 | ASM в 1.25× быстрее |

Комментарий в `spectrum.s` говорит `; = DECAY_FRAMES_FAST` — но значения не совпадают.

Затухание работает быстрее, чем задумано. Визуально эффект менее заметен (бары просто гаснут чуть быстрее), но это нарушает design intent.

### Причина

Значения были скорректированы вручную в ASM, но `variant_cfg.h` не обновлён — или наоборот. ASM не `#include`-ит `.h` файлы, поэтому автоматической синхронизации нет.

### Решение

**Вариант A (рекомендуемый):** Привести `variant_cfg.h` в соответствие с ASM (если текущее поведение визуально устраивает):

```c
#define DECAY_FRAMES_FAST     1   /* уровни >= 6 — каждый TV-кадр       */
#define DECAY_FRAMES_MED      2   /* уровни 3-5  — каждый 2-й кадр      */
#define DECAY_FRAMES_SLOW     4   /* уровни 1-2  — каждый 4-й кадр      */
```

**Вариант B:** Привести ASM в соответствие с `.h`:

```asm
DECAY_FAST  = 2           ; = DECAY_FRAMES_FAST
DECAY_MED   = 3           ; = DECAY_FRAMES_MED
DECAY_SLOW  = 5           ; = DECAY_FRAMES_SLOW
```

**Вариант C:** Добавить `build_variants.ps1` шаг, который генерирует `spectrum_cfg.inc` из `variant_cfg.h` (полная автоматизация).

### Оценка

~5 мин (вариант A или B). ~30 мин (вариант C).

---

## 2. BUG: SAA1099 — нет звука

**Приоритет: ВЫСОКИЙ**  
**Файлы:** `main.c`, `asm/isr.s`, `asm/cmdblocks.asm`

### Симптомы

Спектроанализатор показывает активность (бары двигаются), но звук SAA1099 не воспроизводится. Это подтверждает, что:
- VGM-парсинг корректен (SAA1099 обнаружен)
- `fill_buffer` генерирует `CMD_WRITE_SAA` команды
- `spectrum_saa()` вызывается и устанавливает бары
- Команды попадают в ISR

### Анализ: вероятные причины

#### Причина 1: `CMDBLK_SAA_CLK_ON` не используется в playback queue

Cmdblock `CMDBLK_SAA_CLK_ON` (index 7) определён в `cmdblocks.asm` и `vgm.h`, но **НЕ вызывается** в `build_playback_queue()`:

```c
// main.c:145 — текущий код
if (s_has_opl) {
    hl_push(HLCMD_CMDBLK, CMDBLK_SILENCE_OPL);
    hl_push(HLCMD_CMDBLK, CMDBLK_INIT_OPL...);
}
// ← Нет CMDBLK_SAA_CLK_ON!
hl_push(HLCMD_PLAY, 0);
```

При этом `start_playback()` записывает `isr_ms_ctrl` в порт `#FFFD` напрямую:

```c
isr_ms_ctrl = 0xF2 | (s_has_saa ? 0x00 : 0x08);
// → запись в #FFFD
```

Для SAA-only файла: `isr_ms_ctrl = 0xF6` (SAA CLK ON, FM OFF). Запись происходит **один раз** до запуска ISR.

#### Причина 2: WC ISR может перезаписывать #FFFD

`call_wc_handler()` вызывается ~50 Гц из main loop. WC ISR сканирует клавиатуру через AY-порт `#FFFD`. Если WC записывает значение с `bit7-4 != 0xF` (обычный AY-регистр), MultiSound CPLD может не терять ctrl-состояние. Но если WC записывает `0xFE` или подобное — SAA clock отключится.

**Тест:** добавить `CMDBLK_SAA_CLK_ON` в playback queue для гарантированного включения через ISR (каждый буфер прокатывает ctrl-запись заново).

#### Причина 3: ISR пишет SAA без предварительного ctrl-refresh

`_isr_write_ay` записывает `isr_ms_ctrl` в `#FFFD` перед каждым AY-доступом. Но `_isr_write_saa` **НЕ** записывает ctrl в `#FFFD` — идёт сразу к портам `#00FF/#01FF`.

Если MultiSound CPLD требует ctrl в `#FFFD` для активации SAA-шины, записи в `#00FF` не дойдут до чипа.

#### Причина 4: Sound Enable не записан

SAA1099 требует запись `reg 0x1C = 0x02` для включения звука. Если VGM-файл не содержит этой записи, чип останется немым. Большинство VGM-логгеров записывают enable, но не все.

### Решение (пошаговое)

1. **Добавить `CMDBLK_SAA_CLK_ON` в playback queue** перед `HLCMD_PLAY`:

```c
if (s_has_saa)
    hl_push(HLCMD_CMDBLK, CMDBLK_SAA_CLK_ON);
```

2. **Добавить ctrl-refresh в `_isr_write_saa`** (по аналогии с `_isr_write_ay`):

```asm
_isr_write_saa:
    ld   bc, #PORT_SYSCONF
    ld   a, #TURBO_7MHZ
    out  (c), a
    ;; ← ADD: refresh MultiSound ctrl for SAA bus
    ld   bc, #0xFFFD
    ld   a, (_isr_ms_ctrl)
    out  (c), a
    ;; proceed with SAA write
    ld   a, (de)
    ld   bc, #0x00FF
    ...
```

3. **Создать `CMDBLK_INIT_SAA`** — записать Sound Enable:

```asm
blk_init_saa:
    saa_write #1C, #02     ; Sound enable ON
    blk_end
```

И добавить в playback queue:

```c
if (s_has_saa) {
    hl_push(HLCMD_CMDBLK, CMDBLK_SAA_CLK_ON);
    hl_push(HLCMD_CMDBLK, CMDBLK_INIT_SAA);
}
```

4. Проверить на реальном MultiSound: работает ли SAA после этих изменений.

### Оценка

~2-3 часа (с учётом тестирования на реальном железе).

---

## 3. BUG: YM2203 — тон выше чем надо

**Приоритет: СРЕДНИЙ**  
**Файлы:** `lib/vgm.c`, `inc/freq_lut_map.h`, `scripts/gen_freq_tables.js`

### Симптомы

Тон звука YM2203 SSG-части «явно выше чем надо, возможно раза в два».

### Анализ

#### Архитектура частот

- **AY-3-8910:** `f = clock / (16 × TP)`, где `clock` — прямой PSG-clock
- **YM2203 SSG:** `f = (master_clock / 2) / (16 × TP)` — внутренний делитель ×2
- **VGM header:** для AY8910 (offset 0x74) хранится PSG clock; для YM2203 (offset 0x44) — полный master clock

#### Проверка freq_lut_map:

```c
// YM2203 @ 3580 kHz → PSG effective = 1790 kHz
// Используется та же LUT что и для AY @ 1790:
{ 3580u, 179u, 1, 0x0000 },  /* → page 1, same as AY@1790 */
```

Таблицы ОБЩИЕ: `YM_master / AY_psg = 3580 / 1790 = 2`. Период не меняется, потому что исходный `TP` уже корректен для SSG-части (которая делит master clock на 2). Ratio = `hw_psg / src_psg = 1750 / 1790 = 0.978` — минимальная коррекция.

#### Bypass-проверка:

```c
// HW YM2203 master = 3500 kHz, bypass_tol = 70
// VGM YM2203 @ 3579 kHz: |3579 - 3500| = 79 > 70 → table mode
// VGM YM2203 @ 3500 kHz: |3500 - 3500| = 0 ≤ 70 → native mode
```

В native mode (clocks совпадают) тон должен быть точным. В table mode коррекция ~2% — точнее, чем native. **Оба режима не дают 2× ошибку.**

#### Другие гипотезы

1. **Несоответствие clock на MultiSound:** если реальный AY/YM на плате тактируется частотой, отличной от 3500 kHz / 1750 kHz
2. **Ошибка в gen_freq_tables.js:** деление на 2 для YM2203 может быть применено дважды или не применено
3. **FM-регистры файла интерпретируются как PSG:** для `fb_b1 >= 0x10` вызывается `CMD_WRITE_AY`, что отправляет FM-регистры в AY-чип. На чистом AY эти регистры (> 15) игнорируются, но spectrum_ay вносит мусор в shadow-буферы
4. **Скорость воспроизведения:** ISR_FREQ=683 vs 44100/64=689 → 0.991× — **не 2×**

### Решение

1. **Проверить clock hardware:** измерить реальный тактовый сигнал на MultiSound через осциллограф или `top.v`
2. **Сравнить с эталоном:** проиграть тот же VGM в VGMPlay (ПК) и сравнить тон
3. **Проверить gen_freq_tables.js:** убедиться, что для записи `freq_lut_map_ym` масштаб `= hw_master / src_master`, а не `(hw_master/2) / src_master`
4. **Добавить debug вывод:** в `vgm_parse_header` выводить `clock_khz` и `vgm_freq_mode` для конкретного файла
5. **Фильтр FM-регистров для AY:** если `fb_b1 >= 0x10` и нет реального YM2203 FM — не вызывать `spectrum_ay()` (мусорные shadow-данные → ложные спектро-бары)

### Оценка

~4-6 часов (зависит от доступа к железу).

---

## 4. Анализ: пропуск нот / race conditions

**Приоритет: СРЕДНИЙ**  
**Файлы:** `lib/vgm.c`, `asm/isr.s`, `main.c`

### Текущая защита от race

1. **Двойная буферизация:** ISR читает active, main заполняет free.
2. **`instant_abort()`:** `DI → enabled=0 → HALT → wait_ctr=0 → fill free → switch → enabled=1`.
3. **`CMD_END_BUF`:** ISR переключает active_buf атомарно.
4. **Принудительный SKIP перед END_BUF:** `asm_emit_skip_spec(0) + CMD_END_BUF` в конце каждого буфера — гарантия паузы ≥1 тик перед переключением.

### Потенциальные проблемы

#### 4.1. Buffer starvation (голодание буфера)

Если main loop не успевает заполнить free buffer до того, как ISR доберётся до `CMD_END_BUF` — ISR переключится на **незаполненный** буфер. Там может быть мусор от предыдущей итерации.

**Защита:** `s_buf_ready[]` tracking. Main loop заполняет буфер только если `s_buf_ready[free_idx] == 0`. Но если ISR переключил buf слишком быстро, main loop может пропустить один цикл.

**Симптом:** пропуск группы нот (весь незаполненный буфер = мусор).

**Частота:** редко. Буфер = 128 команд, при budget=16 и ~14 тиков/кадр → буфер хватает на ~9 кадров = ~180 мс. Main loop обычно успевает за 1-2 кадра.

**Уязвимые сценарии:**
- VGZ распаковка очень длинного файла (inflate + load замедляет main loop)
- Много GD3 print_line вызовов при 30+ чипах (теоретически невозможно)

#### 4.2. Budget yield-компенсация неточность

```c
// do_wait:
if (fb_tk > fb_yield_ticks)
    fb_tk -= fb_yield_ticks;
else
    fb_tk = 0;
```

При `fb_yield_ticks > fb_tk` — потерянные тики. Это может привести к микро-ускорению воспроизведения (накопительно), но не к пропуску нот.

#### 4.3. CMD_SKIP_TICKS N > ISR_TICKS_PER_FRAME (разрушительный баг)

Инвариант `N < ISR_TICKS_PER_FRAME` (=14) документирован и определяет максимальное значение для SKIP. Но:

- `emit_wait` гарантирует: если `tk > ISR_TICKS_PER_FRAME`, то `CMD_WAIT` + `CMD_SKIP_TICKS(ISR_TICKS_PER_FRAME-1)` ✓
- `do_wait` inline path: если `fb_tk <= ISR_TICKS_PER_FRAME` → `asm_emit_skip_spec(fb_tk-1)` где `fb_tk-1` ≤ 13 ✓
- `do_budget` yield: `asm_emit_skip_spec(0)` — всегда N=0 ✓

**Вердикт:** инвариант соблюдается во всех путях. Безопасно.

#### 4.4. isr\_tick\_ctr wraparound и decay

`isr_tick_ctr` — uint8, wrap на 256. `spectrum_decay` использует:

```asm
ld   a, (_isr_tick_ctr)
sub  (hl)              ; elapsed = now - last
cp   #ISR_TICKS_PER_FRAME
ret  c                 ; < 1 frame, skip
```

При wrap: `now=5, last=250 → elapsed = 5-250 = 11 (u8 wrap) < 14 → skip`. Это может пропустить один кадр затухания. Но разница 5-250 = 255-250+5+1 = 11 (как unsigned). Actually, SUB в Z80: 5 - 250 = -245 → unsigned = 11. Это 11 < 14, так что один кадр пропустится при wrap. Это безвредно — следующий будет 11+14=25 → обработается чуть позже.

Проблема возникает если elapsed > 14 (пропущено более одного кадра). Decay обрабатывает только ОДИН кадр за вызов (advance by ISR_TICKS_PER_FRAME). При долгой паузе (heavy fill_buffer) несколько кадров будут пропущены. Визуальный эффект: бар «зависает» дольше чем должен, затем постепенно догоняет.

### Вывод

**Реальных race condition с потерей нот не обнаружено.** Двойная буферизация + принудительный SKIP_TICKS перед END_BUF обеспечивают race-free переключение. Единственный теоретический сценарий — buffer starvation, который потребовал бы задержку main loop > 180 мс.

### Оценка

Исследование завершено. Действий не требуется (кроме опционального мониторинга в debug mode).

---

## 5. Оптимизация: spectrum_render — skip пустых баров

**Приоритет: НИЗКИЙ**  
**Файл:** `asm/spectrum.s`

### Текущая стоимость

Внутренний цикл `sr_bar_loop`: 16 баров × 4 строки = 64 итерации.
Каждая итерация:
- Пустой бар: `ld + cp + jr + ld + ld×2 + ld + ld×2 + inc + inc×3 + inc×3 + djnz ≈ 120T`
- Полный/полу бар: `≈ 100T`

Итого: ~64 × 110T ≈ **7040 T** за вызов render.

### Предлагаемая оптимизация

**Идея 1: Skip bar if level==0 AND was 0 before.**

Нужен shadow-массив `spectrum_prev[16]`. После render: `spectrum_prev[i] = spectrum_levels[i]`. Если оба == 0, skip.

Экономия: ~50% итераций при тихих паузах. Cost: 16 байт RAM + 16 × сравнение на entry.

**Идея 2: Column-based rendering (вместо row-based).**

Текущий: 4 прохода по 16 баров (по строкам). IX/IY → char/attr адреса, stride +3 per bar.

Column-based: 16 проходов по 4 строки. Один бар = все 4 строки сразу. Можно skip весь бар если level==0.

Проблема: IX/IY stride между строками = `s_spec_char_ofs[row+1] - s_spec_char_ofs[row]` (зависит от ширины окна, cache-unfriendly). Row-based более cache-friendly для TSConf text mode (строки последовательны в VRAM).

**Вердикт:** идея 1 проще и безопаснее. Идея 2 потенциально не быстрее из-за нелинейного stride.

**Идея 3: Batch early-exit на all-zero.**

Перед render: быстрая проверка: если ВСЕ 16 баров == 0 и shadow == 0 → skip render полностью.

```asm
; Quick all-zero test (16 bytes)
ld   hl, #_spectrum_levels
ld   b, #16
xor  a
.loop:
or   (hl)
inc  hl
djnz .loop
; if A still 0 AND prev also all-zero → ret
```

Стоимость: ~100T. Экономия: ~7000T при полной тишине. Заметный выигрыш.

### Оценка

~1-2 часа.

---

## 6. Оптимизация: spectrum_decay — early-exit

**Приоритет: НИЗКИЙ**  
**Файл:** `asm/spectrum.s`

### Текущая стоимость

Decay-цикл: 16 итераций, каждая ~25T (пустая) до ~40T (с dec). Итого ≈ 400-640T + overhead ≈ **700T**.

### Предлагаемая оптимизация

После frame-gate (ret c → already good), перед bar loop: проверить все 16 уровней на 0.

```asm
; Check if all bars are already 0 — skip decay entirely
ld   hl, #_spectrum_levels
ld   a, (hl)
inc  hl
.rept 15
or   (hl)
inc  hl
.endm
ret  z   ; all zero → nothing to decay
```

~90T. Экономия ~600T на тихих участках.

### Оценка

~20 мин.

---

## 7. Оптимизация: emit_wait — table-based shift

**Приоритет: НИЗКИЙ**  
**Файл:** `lib/vgm.c`

### Текущее состояние

Shift-цепочки в `asm_wait_62/63/short/arb` уже оптимизированы вручную:
- `asm_wait_62`: ~165T (fast-path)
- `asm_wait_63`: ~175T
- `asm_short_wait`: ~175T
- `asm_arb_wait`: ~236T

Это использует bit-rotate + mask вместо DJNZ-цикла. Для `VGM_SAMPLE_SHIFT=6` — всего 2 rotate на байт.

### Table-based альтернатива

LUT 256 байт: `shift_table[x] = x >> 6` (для VGM_SAMPLE_SHIFT=6).

```asm
; HL = sum (16-bit), need HL >> 6
; Table-based: HL = tab[H] * 4 + tab[L]  — нет, сложнее чем кажется
; Для 16-бит сдвига таблица не помогает напрямую.
```

**Вердикт:** текущие rotate+mask цепочки уже оптимальнее, чем 256-byte table для 16-bit shift. Table выигрывает только при SHIFT ≥ 4 на 8-bit значениях. Для 16-бит rotate+mask — **уже оптимальный подход**.

Не рекомендуется менять.

### Оценка

Исследование завершено. Действий не требуется.

---

## 8. UI: наложение метаданных при chip_count > 2

**Приоритет: СРЕДНИЙ**  
**Файл:** `main.c` → `drow_ui()`

### Проблема

При `vgm_chip_count >= 3` строки ChipN занимают rows 7-9+. Вместе с Version (row 6), Loop, Freq и GD3 метаданными — всё упирается в `ROW_VGM_END = 14`.

Область VGM info: строки 6-14 = **9 строк**. Содержимое: 1 (version) + N (chips) + 1 (loop) + 1 (freq) + до 4 (GD3) = 7 + N строк. При N=3: 10 строк → не помещается. При N=5: 12 → GD3 полностью обрезается.

### Решение

Адаптивная разметка: сократить chip info до 1 строки при `chip_count > 2`, или объединить chip entries.

**Вариант A:** Максимум 2 строки chips. При `chip_count > 2` вторая строка: `"+ N more chips"`.

**Вариант B:** Компактный формат для chip_count > 2:

```
Chips: YM3812, AY8910 x2, SAA1099   (1 строка)
```

**Вариант C:** Динамический `ROW_VGM_END` и `DIV2_FROM_BOTTOM` в зависимости от контента.

Рекомендуется **вариант A** — минимальные изменения:

```c
for (uint8_t i = 0; i < vgm_chip_count && row <= ROW_VGM_END; i++) {
    if (i >= 2 && vgm_chip_count > 3) {
        // "  + N more"
        buf_clear(work_buf);
        buf_append_str(work_buf, "              + ");
        buf_append_u16_dec(work_buf, vgm_chip_count - 2);
        buf_append_str(work_buf, " more");
        print_line(&s_wnd, row++, work_buf, CLR_WIN);
        break;
    }
    // ... normal chip line
}
```

### Оценка

~30 мин.

---

## 9. UI: центровка окна по текстовому режиму WC

**Приоритет: НИЗКИЙ**  
**Файл:** `main.c`

### Проблема

Координаты окна захардкожены для TextMode 2 (90×36):

```c
#define WND_X 14   // (90-62)/2 = 14
#define WND_Y 7    // (36-22)/2 = 7
```

В TextMode 0 (80×25): окно начинается с x=14, что смещает его вправо. Y=7 → нижняя часть обрезается (7+22=29 > 25).

### Решение

Вычислять x/y динамически:

```c
uint8_t scr_h = wc_get_height();    // 25/30/36
uint8_t scr_w = (scr_h >= 36) ? 90 : 80;  // WC convention

s_wnd.x = (scr_w - WND_W) / 2;
s_wnd.y = (scr_h > WND_H) ? (scr_h - WND_H) / 2 : 0;
```

Или использовать WC auto-center (`x=255, y=255`), если API поддерживает. Проверить в WC документации.

### Оценка

~15 мин.

---

## 10. Debug: border-flash under #ifdef

**Приоритет: НИЗКИЙ**  
**Файлы:** `main.c`, `asm/isr.s`

### Проблема

Border-отладка сейчас ВСЕГДА включена:

- `main.c:41` — `border()` вызывается в `update_buffer()` (красный = заполнение буфера)
- `isr.s:194` — ISR устанавливает жёлтый бордюр при входе, восстанавливает `isr_border_color` при выходе
- `main.c` — `isr_border_color = 2/0` в `update_buffer()`

### Решение

Обернуть в `#ifdef DEBUG_BORDER`:

```c
/* main.c */
#ifdef DEBUG_BORDER
    isr_border_color = 2;
    border(2);
#endif
    vgm_fill_buffer(free_idx);
#ifdef DEBUG_BORDER
    isr_border_color = 0;
    border(0);
#endif
```

Для ISR (`isr.s`) — условная компиляция сложнее (sdasz80 не поддерживает `#ifdef`). Варианты:
- Вставить `; debug: ld a, #6; out (0xFE), a` как комментарий (ручное включение)
- Использовать `.if` / `.endif` с `.define` в отдельном `debug_cfg.inc`
- Оставить ISR border как есть — он показывает загрузку CPU по ISR, что полезно всегда

**Рекомендация:** обернуть ТОЛЬКО `main.c` border-вызовы. ISR border оставить (1 OUT на вход/выход ISR — пренебрежимая стоимость).

### Оценка

~15 мин.

---

## Сводная таблица

| # | Задача | Приоритет | Оценка | Статус |
|---|--------|-----------|--------|--------|
| 1 | Decay timer mismatch | ВЫСОКИЙ | 5 мин | ❌ |
| 2 | SAA1099 no sound | ВЫСОКИЙ | 2-3 ч | ❌ |
| 3 | YM2203 pitch 2× high | СРЕДНИЙ | 4-6 ч | ❌ |
| 4 | Note drops analysis | СРЕДНИЙ | done | ✅ |
| 5 | spectrum_render skip | НИЗКИЙ | 1-2 ч | ❌ |
| 6 | spectrum_decay early-exit | НИЗКИЙ | 20 мин | ❌ |
| 7 | emit_wait table shift | НИЗКИЙ | done | ✅ |
| 8 | UI chip/metadata overlap | СРЕДНИЙ | 30 мин | ❌ |
| 9 | Window centering | НИЗКИЙ | 15 мин | ❌ |
| 10 | Debug border #ifdef | НИЗКИЙ | 15 мин | ❌ |

**Рекомендуемый порядок:** 1 → 2 → 8 → 9 → 10 → 6 → 5 → 3

---

## Приложение: Техническая справка

### Архитектура потока данных спектроанализатора

```
VGM stream → fill_buffer → spectrum_ay/opl/saa (ASM, shadow regs)
                         → spec_mask (16-bit bitmask)
                         → CMD_SKIP_TICKS [N, mask_lo, mask_hi]
                         → ISR: unpack mask → spectrum_levels[i] = 8
                         → main loop: spectrum_decay (3-timer, frame-gated)
                         → main loop: spectrum_render (4 rows × 16 bars → VRAM)
```

### Тайминги ISR

- ISR_FREQ = 683 Hz (14 позиций × 5120 T = 71680 T/кадр)
- ISR_TICKS_PER_FRAME = 14
- VGM_SAMPLE_SHIFT = 6 (44100 / 64 ≈ 689)
- CMD_BUF_SIZE = 512 байт (128 команд)
- VGM_FILL_CMD_BUDGET = 16 (команд до принудительного yield)

### MultiSound ctrl byte (#FFFD)

```
bit 0: chip select (0=chip1, 1=chip2/TS)
bit 1: normal read (always 1)
bit 2: FM OFF (1=forced low, 0=high-Z=FM ON)
bit 3: SAA CLK OFF (1=off, 0=on)
bit 7-4: 0xF = MS ctrl marker (vs AY register 0x00-0x0F)
```

### Decay timing (ISR_TICKS_PER_FRAME = 14, ~20ms TV-frame)

| Уровень | Таймер | Кадров | Время/шаг | Полное затухание |
|---------|--------|--------|-----------|-----------------|
| 8-6 | FAST (1) | 1 | ~20 ms | 3 × 20 = 60 ms |
| 5-3 | MED (2) | 2 | ~40 ms | 3 × 40 = 120 ms |
| 2-1 | SLOW (4) | 4 | ~80 ms | 2 × 80 = 160 ms |

Итого падение 8→0: ~60 + 120 + 160 = **~340 мс** (по ASM-значениям 1/2/4).

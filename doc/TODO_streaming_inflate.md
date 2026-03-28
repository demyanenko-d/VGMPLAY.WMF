# TODO: Потоковая (streaming) распаковка inflate

**Дата:** 2026-03-28  
**Статус:** Идея / Анализ завершён, реализация отложена

## Цель

Заменить текущую полную предварительную распаковку VGZ → мегабуфер
на потоковую (чанковую) распаковку с прерываниями EI, что даст:

- Мгновенный старт воспроизведения (не ждать 2–10 сек)
- Снятие ограничения 1 МБ (мегабуфер) на размер raw VGM
- Возможность играть VGZ файлы > 496 КБ (TAP) через кольцевой буфер

## Ключевые находки

### 1. ISR не использует Win 0 / Win 1 / Win 3

ISR-хэндлер и все данные (cmd_buf_a/b, isr_read_ptr, pos_ptr) живут
исключительно в Win 2 (0x8000–0xBFFF, _CODE + _DATA). Это означает:

**Win 0, Win 1, Win 3 можно свободно переключать при EI.**

### 2. Раскладка окон во время inflate-чанка

| Win | Адреса          | Содержимое                | Кто использует           |
|-----|-----------------|---------------------------|--------------------------|
| 0   | 0x0000–0x3FFF   | Source VGZ page (ротация) | inflate: чтение HL'      |
| 1   | 0x4000–0x7FFF   | Decode buffer (PAGE_DECODE)| inflate: запись DE        |
| 2   | 0x8000–0xBFFF   | Player code + ISR + DATA  | **НЕ ТРОГАЕМ**, ISR тут  |
| 3   | 0xC000–0xFFFF   | Inflate code + Huffman    | inflate: исполнение      |

Win 2 сейчас занят decode buffer (#EA) во время inflate.
Для streaming нужно перенести decode buffer из Win 2 в Win 1.

### 3. Self-modifying code = бесплатное сохранение состояния

Все переменные inflate (`src_page`, `dst_page`, `dst`, `interim_dst`,
`this_was_last_block`, Huffman таблицы ~10 КБ) — self-modifying code
на inflate-странице (Win 3). При unmap/remap страницы значения
автоматически сохраняются. Стек (SP = 0xFFF0, адреса возврата
inflate_core → blocktype2 → decode_tableloop) тоже на inflate-странице.

**Нужно сохранять только CPU-регистры (18 байт):**
- AF, BC, DE, HL (8 байт main regs)
- AF', BC', DE', HL' (8 байт alt regs — bit-stream state!)
- IX (2 байта)
- SP хранится в C-переменной caller

### 4. DMA вместо LDIR для page flush

Текущий `copy_decoded_page` (LDIR 16 КБ) = **344064 T** ≈ 49 мс @ 7 МГц.

DMA RAM→RAM (TSCONF_DMA_RAM_RAM = 0x09): **~8000–16000 T** (20–40× быстрее).
Адресация: 22-bit [page(8)][H(6)][L(7)][0].
DMA копирует страницу→страницу без участия CPU и без маппинга окон.

Двойной буфер DMA: пока DMA копирует страницу N, inflate пишет в страницу N+1.
Не нужно ждать флаг TSCONF_DMA_ACT.

### 5. Backref: DMA для cross-page, LDIR для intra-page

~80–90% backrefs внутри текущего буфера → LDIR из Win 1 в Win 1.

Cross-page backrefs (distance > current_offset): DMA из физической
страницы history → физическую страницу decode buffer. Окна не трогаем.

Исключение: overlapping backrefs (distance < length, паттерн "repeat") 
DMA не гарантирует побайтную семантику → fallback: temp remap Win 0 = history,
LDIR, restore Win 0 = src.

### 6. Overflow 258 байт — решение для Win 1

Если decode buffer в Win 1 (0x4000–0x7FFF), overflow LDIR пойдёт
в Win 2 (player code). **Нельзя.**

Решение: split-copy перед LDIR. Проверить DE + BC > 0x8000?
Если да — LDIR до границы, DMA flush, сброс DE = 0x4000, LDIR остаток.
Overhead: ~30 T на backref (проверка).

### 7. Yield point: decode_tableloop

Единственная разумная точка yield — вершина `decode_tableloop`.
Добавить yield_counter (self-mod var): dec → jp z, inflate_yield.
Caller задаёт количество 8-символьных батчей за чанк.

### 8. Оценка бюджета

| Параметр                         | Значение                 |
|----------------------------------|--------------------------|
| Потребление VGM (типичный)       | ~2.5 КБ/сек (50 б/кадр) |
| Скорость inflate @ 7 МГц        | ~50–100 КБ/сек           |
| Нужный duty cycle                | **2.5–5% CPU**           |
| Overhead контекста (вход+выход)  | ~600 T / 71680 T = 0.8%  |
| Запас                            | 10–20×                   |

### 9. Кольцевой буфер выхода

```
Megabuffer pages: [P0][P1][P2][P3][P4][P5][P6][P7] ... ring

write_ptr → inflate пишет (producer)
read_ptr  → player читает (consumer)
```

- Старт воспроизведения после первых 2–4 готовых страниц
- Нет ограничения 1 МБ: ring buffer кольцуется
- Max VGM raw: 496 КБ × 3 (сжатие) ≈ 1.5 МБ

## Контекст переключения (pseudo-code)

```asm
inflate_enter:
    ; Save Win 0/1/3 page numbers (from C vars or sys vars)
    ; OUT #10AF = src_page        ; Win 0 = source VGZ
    ; OUT #11AF = PAGE_DECODE     ; Win 1 = decode buffer
    ; OUT #13AF = inflate_page    ; Win 3 = inflate code
    ; Restore saved CPU regs (18 bytes from save area on inflate page)
    ; JP resume_point

inflate_yield:
    ; Save CPU regs to save area on inflate page
    ; ld sp, (caller_sp)
    ; Restore Win 0/1/3 from saved values
    ; ret to main loop
```

## План реализации

1. [ ] Перенести decode buffer из Win 2 в Win 1
2. [ ] Добавить split-copy проверку для LDIR overflow в Win 1
3. [ ] Реализовать yield/resume в decode_tableloop
4. [ ] Заменить LDIR page flush на DMA
5. [ ] Реализовать DMA backref для cross-page
6. [ ] Добавить кольцевой буфер (producer/consumer) в main.c
7. [ ] Добавить inflate_enter / inflate_yield в inflate_call.s
8. [ ] Тестирование с файлами разных размеров и степеней сжатия

## Риски

- makeHuffmanTable: может занять десятки тысяч T-states (непредсказуемо).
  При yield внутри него нужен глубокий стек save. Альтернатива:
  не yield'ить во время построения таблиц, только во время decode.
- DMA overlap с ISR: ISR не использует DMA, конфликтов нет.
- Первый блок может быть dynamic Huffman → долгое построение таблиц перед
  первыми декодированными данными. Mitigation: буферизовать 2–4 страницы
  перед стартом воспроизведения.

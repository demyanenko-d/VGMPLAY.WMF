# План: новые ISR-команды и оптимизация

## Проблема
ISR вызывается 2734 раз/сек. При эфф. ~9 МГц бюджет ~3292T/тик.
Текущий overhead ~322T оставляет ~2970T на команды.
Но большую часть времени ISR просто декрементирует wait_ctr
(VGM-файлы: burst регистровых записей, потом 45-55 тиков паузы).
Во время пауз ISR тратит ~320T×45 = 14400T/кадр впустую.
Плюс main loop не получает WC-таймер → клавиатура тупит.

## Решение: 3 новые ISR-команды

### CMD_INC_SEC (0x10)
Формат: `[0x10, 0, 0, 0]`
ISR: `isr_play_seconds++`. Вставляется buffer filler каждые ISR_FREQ тиков.
Стоимость: ~40T (только при выполнении, ~раз в секунду).

### CMD_CALL_WC (0x30)
Формат: `[0x30, skip_count, 0, 0]`
ISR: пропустить skip_count записей в pos_table (переписать порты INT),
     сохранить read_ptr, POP регистров, JP на WC-обработчик.
WC-обработчик делает EI+RET → возврат в прерванный код.
Эффект: следующие skip_count тиков ISR НЕ вызывается → CPU свободен.
skip_count: 1..55 (макс кадр-1). Типичное значение: 40-50.
Вставляется buffer filler при длинных паузах, ~раз в кадр (50 Гц).

Техника JP на WC без потери HL:
```
    pop de / pop bc / pop hl / pop af
    push hl
    ld hl, (_isr_saved_vec)
    ex (sp), hl          ; stack=[WC_addr], HL=restored
    ret                   ; PC=WC_addr, stack=[ret_from_int]
```

### CMD_SKIP_TICKS (сливается с CMD_CALL_WC)
Не нужна отдельная — CMD_CALL_WC сам пропускает тики.

## Изменения по файлам

### Шаг 1: isr.h — новые коды и isr_play_seconds
- Добавить CMD_INC_SEC = 0x10, CMD_CALL_WC = 0x30
- Вернуть extern isr_play_seconds

### Шаг 2: isr.s — новые обработчики
- Добавить .globl _isr_play_seconds
- Добавить _isr_play_seconds в DATA
- Инициализировать isr_play_seconds=0 в isr_init
- Dispatch: перед END_BUF fallthrough добавить CP CMD_INC_SEC / CMD_CALL_WC
- _isr_inc_sec: inc hl 16-bit, jp _isr_cmd_loop (40T)
- _isr_call_wc: skip loop + reprogram ports + save read_ptr + JP WC

### Шаг 3: vgm.c — emit_wait_smart
- Новые глобальные переменные:
  - vgm_sec_budget (uint16_t): тиков до след. CMD_INC_SEC, init=ISR_FREQ
  - vgm_wc_budget (uint8_t): тиков до след. CMD_CALL_WC, init=56
- Функция emit_wait(buf, &off, ticks):
  while (ticks > 0):
    a) если sec_budget <= ticks: emit CMD_WAIT(sec_budget), CMD_INC_SEC,
       ticks -= sec_budget, sec_budget = ISR_FREQ, wc_budget -= min(sec_budget, wc_budget)
    b) если wc_budget <= ticks && wc_budget >= 4:
       skip = min(wc_budget - 1, 55), emit CMD_CALL_WC(skip),
       ticks -= skip, wc_budget = 56 - skip    (reset ~1 кадр)
    c) emit CMD_WAIT(ticks), sec_budget -= ticks, wc_budget -= ticks, break
- Все места где сейчас emit CMD_WAIT → вызов emit_wait

### Шаг 4: main.c — убрать локальные секунды
- Убрать s_tick_prev, s_tick_accum, s_play_seconds, update_seconds()
- Использовать isr_play_seconds для отображения времени
- Оставить isr_tick_ctr для polling клавиатуры (~50 Гц)

### Шаг 5: build + verify

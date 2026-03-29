/**
 * @file  variant_cfg.h
 * @brief Единственный файл конфигурации варианта сборки.
 *
 * build_variants.ps1 перегенерирует этот файл целиком для каждого
 * варианта.  Никакие другие исходники НЕ патчатся.
 *
 * build.bat читает VARIANT_POS_ENTRIES / VARIANT_POS_STEP /
 * VARIANT_DATA_LOC из этого файла (парсинг через for /f).
 */
#ifndef VARIANT_CFG_H
#define VARIANT_CFG_H

/* ── Частота ISR (тиков/сек) ─────────────────────────────────────── */
/* ISR_FREQ = 3 500 000 / VARIANT_POS_STEP                            */
#define ISR_FREQ              683

/* Тиков ISR на TV-кадр (= 71680 / STEP)                              */
/* 14@683, 28@1367, 56@2734                                            */
#define ISR_TICKS_PER_FRAME   14

/* ── Маппинг VGM 44100→ISR тиков ─────────────────────────────────── */
/* SHIFT = log2(44100/ISR_FREQ): 6@683, 5@1367, 4@2734                */
#define VGM_SAMPLE_SHIFT      6
#define VGM_SAMPLE_MASK       0x3Fu

/* ── Лимит команд между принудительными yield ──────────────────────── */
#define VGM_FILL_CMD_BUDGET   32

/* ── Параметры pos_table (для build.bat → gen_pos_table.js) ──────── */
#define VARIANT_POS_ENTRIES   14
#define VARIANT_POS_STEP      5120

/* ── Адрес начала сегмента данных (для линковщика) ────────────────── */
#define VARIANT_DATA_LOC      0xB600

/* ── Отключение обработки коротких пауз 0x70-0x7F ────────────────── */
/* Раскомментировать для «no short waits» варианта:                    */
/* #define VGM_NO_SHORT_WAITS */

#endif /* VARIANT_CFG_H */
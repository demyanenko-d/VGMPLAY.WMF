/**
 * @file    tsconf.h
 * @brief   Полный справочник регистров TSConfig (TSL FPGA-ускоритель ZX Spectrum)
 * @version 2.0
 *
 * Самодостаточная библиотека: зависит только от types.h.
 * Подходит для любого TSConfig-проекта на SDCC Z80.
 *
 * ═══════════════════════════════════════════════════════════════════
 * АРХИТЕКТУРА ПОРТОВ
 * ═══════════════════════════════════════════════════════════════════
 *
 * Все порты TSConfig адресуются по схеме: (REG_NUM << 8) | 0xAF
 * Запись: OUT (C),A  где BC = порт.
 *
 * На ASM:
 *   LD BC, TSCONF_PORT_SYSCONFIG   ; BC = 0x20AF
 *   LD A, TSCONF_CPU_14MHZ
 *   OUT (C), A
 *
 * На C:
 *   tsconf_out(TSCONF_PORT_SYSCONFIG, TSCONF_CPU_14MHZ);
 *
 * Макросы-обёртки (конец файла) упрощают типичные операции:
 *   tsconf_set_cpu(TSCONF_CPU_14MHZ);
 *   tsconf_set_border(1);
 *   tsconf_set_frame_int(100, 0);
 *
 * ═══════════════════════════════════════════════════════════════════
 * ТАЙМИНГИ TV-РЕЖИМА (Pentagon-128)
 * ═══════════════════════════════════════════════════════════════════
 *
 * Строк в кадре: 320 (0–319)
 *   Строки  0–31:   вертикальное гашение
 *   Строки 32–319:  видимая область (бордер + пиксели)
 *
 * Пикселей в строке: 448 (7 МГц), = 224 T-states при 3.5 МГц
 *   Пиксели   0–87:   горизонтальное гашение
 *   Пиксели 88–447:  видимая область
 *
 * T-states в кадре: 320 × 224 = 71680 T
 *
 * ══════════════════════════════════════════════════════════════════════
 * ПРЕРЫВАНИЯ (IM2)
 * ══════════════════════════════════════════════════════════════════════
 *
 * Три источника, каждый выставляет свой вектор на шину данных:
 *   FRAME (#FF, приоритет 0) — совпадение растровых счётчиков
 *                              с регистрами HSINT/VSINT
 *   LINE  (#FD, приоритет 1) — каждая строка при HCNT = 0
 *   DMA   (#FB, приоритет 2) — конец DMA-транзакции
 *
 * HSINT = HCNT[8:1]: горизонтальная позиция (0–223). >223 = выкл.
 * VSINTL/VSINTH = VCNT[8:0]: вертикальная строка (0–319). >319 = выкл.
 *
 * VINT_INC[3:0] в VSINTH: автоинкремент VCNT после каждого FRAME-INT
 * (позволяет менять строку прерывания без записи в порты).
 *
 * ═══════════════════════════════════════════════════════════════════
 * ВИДЕО
 * ═══════════════════════════════════════════════════════════════════
 *
 * VM[1:0]:  00=ZX (256x192, атрибуты), 01=16c, 10=256c, 11=txt
 * RRES[1:0]: 00=256x192, 01=320x200, 10=320x240, 11=360x288
 *
 * ═══════════════════════════════════════════════════════════════════
 * DMA
 * ═══════════════════════════════════════════════════════════════════
 *
 * Адрес = 22-бит: [X(8)][H(6)][L(7)][0] — младший бит всегда 0.
 *
 * ═══════════════════════════════════════════════════════════════════
 * ПРИМЕР (минимальный)
 * ═══════════════════════════════════════════════════════════════════
 *
 *   #include "tsconf.h"
 *
 *   void demo(void) {
 *       tsconf_set_cpu(TSCONF_CPU_14MHZ);          // 14 МГц
 *       tsconf_set_border(2);                       // красный бордюр
 *       tsconf_set_vconfig(TSCONF_VM_ZX             // ZX-режим
 *                        | TSCONF_RRES_256x192);    // 256x192
 *       tsconf_set_frame_int(100, 0);               // INT на строке 100
 *       tsconf_set_intmask(TSCONF_INT_FRAME);       // только FRAME
 *       tsconf_set_cpu(TSCONF_CPU_3_5MHZ);          // вернуть 3.5
 *   }
 */

#ifndef TSCONF_H
#define TSCONF_H

#include "types.h"

/* ══════════════════════════════════════════════════════════════════════
 * Макрос формирования адреса порта
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT(reg)    (((uint16_t)(reg) << 8) | 0xAF)

/* ══════════════════════════════════════════════════════════════════════
 * ВИДЕО — VConfig (#00AF)
 * W: RRES[1:0] | - | NOGFX | NOTSU | GFXOVR* | FT_EN** | VM[1:0]
 * R: - | PWR_UP | - | FDRVER | VDVER[2:0]
 * * только VDAC,  ** только VDAC2.  Сброс: 0x00
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_VCONFIG     TSCONF_PORT(0x00)

/* VM[1:0] — видеорежим */
#define TSCONF_VM_ZX            0x00    /* ZX-Spectrum, 256×192, атрибуты */
#define TSCONF_VM_16C           0x01    /* 16 цветов, 4 бита/пиксель      */
#define TSCONF_VM_256C          0x02    /* 256 цветов, 8 бит/пиксель      */
#define TSCONF_VM_TXT           0x03    /* Текстовый режим                */
#define TSCONF_VM_MASK          0x03

/* FT_EN, GFXOVR, NOTSU, NOGFX */
#define TSCONF_VC_FT_EN         0x04    /* FT812 enable (VDAC2)           */
#define TSCONF_VC_GFXOVR        0x08    /* GFX поверх TSU (VDAC)          */
#define TSCONF_VC_NOTSU         0x10    /* Отключить TSU-графику           */
#define TSCONF_VC_NOGFX         0x20    /* Отключить основную графику      */

/* RRES[1:0] — разрешение экрана */
#define TSCONF_RRES_256x192     (0x00 << 6)
#define TSCONF_RRES_320x200     (0x01 << 6)
#define TSCONF_RRES_320x240     (0x02 << 6)
#define TSCONF_RRES_360x288     (0x03 << 6)
#define TSCONF_RRES_MASK        (0x03 << 6)

/* R: Status (то же адрес #00AF, чтение) */
#define TSCONF_STATUS_VDVER_MASK    0x07
#define TSCONF_STATUS_FDRVER        0x10
#define TSCONF_STATUS_PWR_UP        0x40
#define TSCONF_VDVER_2BIT_PWM       0x00
#define TSCONF_VDVER_3BIT           0x01
#define TSCONF_VDVER_4BIT           0x02
#define TSCONF_VDVER_5BIT           0x03
#define TSCONF_VDVER_VDAC2          0x07

/* ── VPage (#01AF): адрес видеостраницы RA[21:14] ─────────────────── */
#define TSCONF_PORT_VPAGE       TSCONF_PORT(0x01)

/* ── GXOffsL (#02AF), GXOffsH (#03AF): X-смещение графики ─────────── */
#define TSCONF_PORT_GXOFFSL     TSCONF_PORT(0x02)   /* XO[7:0]            */
#define TSCONF_PORT_GXOFFSH     TSCONF_PORT(0x03)   /* бит 0 = XO[8]      */

/* ── GYOffsL (#04AF), GYOffsH (#05AF): Y-смещение графики ─────────── */
#define TSCONF_PORT_GYOFFSL     TSCONF_PORT(0x04)   /* YO[7:0]            */
#define TSCONF_PORT_GYOFFSH     TSCONF_PORT(0x05)   /* бит 0 = YO[8]      */

/* ── TSConfig (#06AF): включение tile/sprite движка ────────────────── */
#define TSCONF_PORT_TSCONFIG    TSCONF_PORT(0x06)
#define TSCONF_TS_EXT           0x01    /* TSU 360×288 (не ZX-Evo)        */
#define TSCONF_T0Z_EN           0x04    /* Тайл [0] виден (плоск. 0)      */
#define TSCONF_T1Z_EN           0x08    /* Тайл [0] виден (плоск. 1)      */
#define TSCONF_T0_EN            0x20    /* Tile-layer 0 enable             */
#define TSCONF_T1_EN            0x40    /* Tile-layer 1 enable             */
#define TSCONF_S_EN             0x80    /* Sprite layers enable            */

/* ── PalSel (#07AF): выбор палитр ──────────────────────────────────── */
/* GPAL[7:4] | T0PAL[7:6] | T1PAL[7:6]. Сброс = 0x0F                  */
#define TSCONF_PORT_PALSEL      TSCONF_PORT(0x07)

/* ── Border (#0FAF): цвет бордюра ──────────────────────────────────── */
#define TSCONF_PORT_BORDER      TSCONF_PORT(0x0F)

/* ══════════════════════════════════════════════════════════════════════
 * УПРАВЛЕНИЕ ПАМЯТЬЮ
 * ══════════════════════════════════════════════════════════════════════
 * RA[21:14] — номер 16KB-страницы DRAM
 * Page0 = W (write-only), Page1 = W, Page2 = RW, Page3 = RW
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_PAGE0       TSCONF_PORT(0x10)   /* окно #0000, W      */
#define TSCONF_PORT_PAGE1       TSCONF_PORT(0x11)   /* окно #4000, W      */
#define TSCONF_PORT_PAGE2       TSCONF_PORT(0x12)   /* окно #8000, RW     */
#define TSCONF_PORT_PAGE3       TSCONF_PORT(0x13)   /* окно #C000, RW     */

/* ── FMAddr (#14AF): адрес Font Maps ───────────────────────────────── */
#define TSCONF_PORT_FMADDR      TSCONF_PORT(0x14)
#define TSCONF_FMADDR_EN        0x10    /* бит 4 = FM_EN. Биты 3:0 = A[15:12] */

/* ── TMPage (#15AF): страница TileMap ──────────────────────────────── */
#define TSCONF_PORT_TMPAGE      TSCONF_PORT(0x15)   /* RA[21:14]          */

/* ── T0GPage (#16AF): страница графики тайлов плоскости 0 ──────────── */
#define TSCONF_PORT_T0GPAGE     TSCONF_PORT(0x16)   /* RA[21:17]          */

/* ── T1GPage (#17AF): страница графики тайлов плоскости 1 ──────────── */
#define TSCONF_PORT_T1GPAGE     TSCONF_PORT(0x17)   /* RA[21:17]          */

/* ── SGPage (#18AF): страница графики спрайтов ─────────────────────── */
#define TSCONF_PORT_SGPAGE      TSCONF_PORT(0x18)   /* RA[21:17]          */

/* ══════════════════════════════════════════════════════════════════════
 * DMA — адреса источника и назначения
 * Адрес = 22-бит: [X(8)][H(6)][L(7)][0]
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_DMASADDRL   TSCONF_PORT(0x19)   /* Source RA[7:1]     */
#define TSCONF_PORT_DMASADDRH   TSCONF_PORT(0x1A)   /* Source RA[13:8]    */
#define TSCONF_PORT_DMASADDRX   TSCONF_PORT(0x1B)   /* Source RA[21:14]   */
#define TSCONF_PORT_DMADADDRL   TSCONF_PORT(0x1C)   /* Dest   RA[7:1]     */
#define TSCONF_PORT_DMADADDRH   TSCONF_PORT(0x1D)   /* Dest   RA[13:8]    */
#define TSCONF_PORT_DMADADDRX   TSCONF_PORT(0x1E)   /* Dest   RA[21:14]   */
#define TSCONF_PORT_DMAWPDEV    TSCONF_PORT(0x25)   /* DMA Wait Port Dev  */
#define TSCONF_PORT_DMALEN      TSCONF_PORT(0x26)   /* Burst Length [7:0] */
#define TSCONF_PORT_DMACTRL     TSCONF_PORT(0x27)   /* Ctrl W / Status R  */
#define TSCONF_PORT_DMANUM      TSCONF_PORT(0x28)   /* Bursts Num [7:0]   */
#define TSCONF_PORT_DMANUMH     TSCONF_PORT(0x2C)   /* Bursts Num [9:8]   */
#define TSCONF_PORT_DMAWPADDR   TSCONF_PORT(0x2D)   /* Wait Port Addr     */

/* DMACtrl W — DDEV[3:0]: режим DMA */
#define TSCONF_DMA_RAM_RAM      0x09    /* RAM→RAM copy                   */
#define TSCONF_DMA_BLT1_RAM     0x89    /* RAM→RAM, пропуск нулей (OPT=1) */
#define TSCONF_DMA_FILL         0x21    /* Fill: src → dest*N             */
#define TSCONF_DMA_RAM_CRAM     0xA1    /* RAM→CRAM (палитра)             */
#define TSCONF_DMA_SPI_RAM      0x11    /* SPI→RAM                        */
#define TSCONF_DMA_RAM_SPI      0x91    /* RAM→SPI                        */
#define TSCONF_DMA_IDE_RAM      0x19    /* IDE→RAM                        */
#define TSCONF_DMA_RAM_IDE      0x99    /* RAM→IDE                        */
#define TSCONF_DMA_FDD_RAM      0x29    /* FDD→RAM                        */
#define TSCONF_DMA_RAM_WTP      0xB9    /* RAM→Wait Port                  */
#define TSCONF_DMA_WTP_RAM      0x39    /* Wait Port→RAM                  */
#define TSCONF_DMA_BLT2_RAM     0x31    /* Блиттер с аддером              */

/* DMACtrl — флаги */
#define TSCONF_DMA_ASZ_256      0x00    /* Выравнивание 256 байт          */
#define TSCONF_DMA_ASZ_512      0x04    /* Выравнивание 512 байт          */
#define TSCONF_DMA_D_ALGN       0x08    /* Выравнивание назначения        */
#define TSCONF_DMA_S_ALGN       0x10    /* Выравнивание источника         */
/* R: DMAStatus */
#define TSCONF_DMA_ACT          0x80    /* DMA активна                    */

/* DMAWPDev */
#define TSCONF_DMAWP_GLUCLOCK   0x00
#define TSCONF_DMAWP_COMPORT    0x01

/* ══════════════════════════════════════════════════════════════════════
 * СИСТЕМНАЯ КОНФИГУРАЦИЯ
 * ══════════════════════════════════════════════════════════════════════ */

/* ── SysConfig (#20AF): частота CPU и кэш ──────────────────────────── */
#define TSCONF_PORT_SYSCONFIG   TSCONF_PORT(0x20)
#define TSCONF_CPU_3_5MHZ       0x00
#define TSCONF_CPU_7MHZ         0x01
#define TSCONF_CPU_14MHZ        0x02
#define TSCONF_CPU_MASK         0x03
#define TSCONF_CACHE_EN         0x04    /* Включить кэш (все 4 окна)      */

/* ── MemConfig (#21AF) ────────────────────────────────────────────────
 * ROM128:  бит 0 = то же что бит 4 в #7FFD (BootROM/Basic)
 * W0_WE:   разрешить запись в окно #0000
 * !W0_MAP: 0=маппинг через Page0, 1=стандартная страница 0
 * W0_RAM:  0=ROM, 1=RAM в #0000
 * LCK128:  режим блокировки 128K-совместимости
 * ──────────────────────────────────────────────────────────────────── */
#define TSCONF_PORT_MEMCONFIG   TSCONF_PORT(0x21)
#define TSCONF_MEM_ROM128       0x01
#define TSCONF_MEM_W0_WE        0x02
#define TSCONF_MEM_W0_MAP_DIS   0x04
#define TSCONF_MEM_W0_RAM       0x08
#define TSCONF_MEM_LCK128_512K  0x00    /* 512K, биты 7:6 #7FFD=Page3[4:3] */
#define TSCONF_MEM_LCK128_128K  0x40    /* 128K, биты 7:6 #7FFD=00          */
#define TSCONF_MEM_LCK128_AUTO  0x80    /* Auto (!a[13])                     */
#define TSCONF_MEM_LCK128_1M    0xC0    /* 1024K                             */

/* ══════════════════════════════════════════════════════════════════════
 * ПРЕРЫВАНИЯ — позиция FRAME-INT
 * ══════════════════════════════════════════════════════════════════════ */

/* ── HSINT (#22AF): горизонтальная позиция ─────────────────────────── */
/* HCNT[8:1] в диапазоне 0–223; >223 = выключить INT в этой строке     */
#define TSCONF_PORT_HSINT       TSCONF_PORT(0x22)
#define TSCONF_HSINT_OFF        0xFF

/* ── VSINTL (#23AF): вертикальная позиция [7:0] ────────────────────── */
/* VCNT[7:0]; строки 0–319; >319 = FRAME-INT выключен                  */
#define TSCONF_PORT_VSINTL      TSCONF_PORT(0x23)

/* ── VSINTH (#24AF): вертикальная позиция [8] + автоинкремент ──────── */
/* Бит 0 = VCNT[8]; биты [7:4] = VINT_INC[3:0]                        */
/* VINT_INC: после каждого FRAME-INT → VCNT += VINT_INC (0 = нет)      */
#define TSCONF_PORT_VSINTH      TSCONF_PORT(0x24)
#define TSCONF_VSINTH_VCNT8     0x01
#define TSCONF_VSINTH_INC_MASK  0xF0
#define TSCONF_VSINTH_INC(n)    (((n) & 0x0F) << 4)

/* ── INTMask (#2AAF): маска прерываний ─────────────────────────────── */
/* Сброс: 0x01 (только FRAME включён)                                  */
#define TSCONF_PORT_INTMASK     TSCONF_PORT(0x2A)
#define TSCONF_INT_FRAME        0x01
#define TSCONF_INT_LINE         0x02
#define TSCONF_INT_DMA          0x04
#define TSCONF_INT_WTP          0x08

/* Векторы IM2 (значение на шине данных при прерывании) */
#define TSCONF_IVEC_FRAME       0xFF
#define TSCONF_IVEC_LINE        0xFD
#define TSCONF_IVEC_DMA         0xFB

/* ══════════════════════════════════════════════════════════════════════
 * КЭШ — CacheConfig (#2BAF)
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_CACHECONFIG TSCONF_PORT(0x2B)
#define TSCONF_CACHE_EN_0000    0x01
#define TSCONF_CACHE_EN_4000    0x02
#define TSCONF_CACHE_EN_8000    0x04
#define TSCONF_CACHE_EN_C000    0x08
#define TSCONF_CACHE_ALL        0x0F
#define TSCONF_CACHE_NONE       0x00

/* ══════════════════════════════════════════════════════════════════════
 * FDD VIRTUAL DRIVE — FDDVirt (#29AF)
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_FDDVIRT     TSCONF_PORT(0x29)
#define TSCONF_FDD_A            0x01
#define TSCONF_FDD_B            0x02
#define TSCONF_FDD_C            0x04
#define TSCONF_FDD_D            0x08
#define TSCONF_FDD_VG_OPEN      0x80

/* ══════════════════════════════════════════════════════════════════════
 * ТАЙЛОВЫЕ ПЛОСКОСТИ — смещения
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_T0XOFFSL    TSCONF_PORT(0x40)
#define TSCONF_PORT_T0XOFFSH    TSCONF_PORT(0x41)
#define TSCONF_PORT_T0YOFFSL    TSCONF_PORT(0x42)
#define TSCONF_PORT_T0YOFFSH    TSCONF_PORT(0x43)
#define TSCONF_PORT_T1XOFFSL    TSCONF_PORT(0x44)
#define TSCONF_PORT_T1XOFFSH    TSCONF_PORT(0x45)
#define TSCONF_PORT_T1YOFFSL    TSCONF_PORT(0x46)
#define TSCONF_PORT_T1YOFFSH    TSCONF_PORT(0x47)

/* ══════════════════════════════════════════════════════════════════════
 * HUS — Hurricane Sound Engine (#80AF–#87AF)
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_HUSCTRL     TSCONF_PORT(0x80)   /* W: управление       */
#define TSCONF_PORT_HUSSAMPRATE TSCONF_PORT(0x81)   /* W: SR[7:0] (1750k/(SR+1)) */
#define TSCONF_PORT_HUSTICKRATEL TSCONF_PORT(0x82)  /* W: TR[7:0]          */
#define TSCONF_PORT_HUSTICKRATEH TSCONF_PORT(0x83)  /* W: TR[9:8]          */
#define TSCONF_PORT_HUSRLD0     TSCONF_PORT(0x84)   /* Reload ch 0–7       */
#define TSCONF_PORT_HUSRLD1     TSCONF_PORT(0x85)   /* Reload ch 8–15      */
#define TSCONF_PORT_HUSRLD2     TSCONF_PORT(0x86)
#define TSCONF_PORT_HUSRLD3     TSCONF_PORT(0x87)
/* R: HUSStatus (#80AF) */
#define TSCONF_HUS_ACT          0x01
#define TSCONF_HUS_TICK         0x02    /* сбрасывается при чтении         */
#define TSCONF_HUS_HALF         0x40    /* буфер наполовину пуст           */
#define TSCONF_HUS_EMPTY        0x80
/* W: HUSControl */
#define TSCONF_HUS_PAUSE        0x02
#define TSCONF_HUS_RESET        0x80

/* ══════════════════════════════════════════════════════════════════════
 * OPL3 (YMF262) — порты на шине TSConfig
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_OPL3_ADDR0  0x00C4
#define TSCONF_PORT_OPL3_DATA0  0x00C5
#define TSCONF_PORT_OPL3_ADDR1  0x00C6
#define TSCONF_PORT_OPL3_DATA1  0x00C7

/* ══════════════════════════════════════════════════════════════════════
 * AY-3-8910 / YM2149 — стандартные порты ZX Spectrum 128
 * ══════════════════════════════════════════════════════════════════════
 *   #FFFD — выбор регистра (W) / чтение данных (R)
 *   #BFFD — запись данных (W)
 *
 * TurboSound (два AY): запись в #FFFD значения #FF/#FE выбирает чип:
 *   #FF → чип 0 (первый AY);  #FE → чип 1 (второй AY).
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_AY_ADDR     0xFFFD  /* register select / read        */
#define TSCONF_PORT_AY_DATA     0xBFFD  /* data write                    */
#define TSCONF_AY_TS_CHIP0      0xFF    /* TurboSound: select chip 0     */
#define TSCONF_AY_TS_CHIP1      0xFE    /* TurboSound: select chip 1     */

/* ══════════════════════════════════════════════════════════════════════
 * SAA1099 — порты SAM Coupé / ZX-совместимые
 * ══════════════════════════════════════════════════════════════════════
 *   #01FF — выбор регистра (address write)
 *   #00FF — запись данных   (data write)
 *   Второй SAA1099 (если есть):
 *   #01FE — выбор регистра
 *   #00FE — запись данных
 * ══════════════════════════════════════════════════════════════════════ */
#define TSCONF_PORT_SAA_ADDR    0x01FF  /* SAA1099 #1 address            */
#define TSCONF_PORT_SAA_DATA    0x00FF  /* SAA1099 #1 data               */
#define TSCONF_PORT_SAA2_ADDR   0x01FE  /* SAA1099 #2 address            */
#define TSCONF_PORT_SAA2_DATA   0x00FE  /* SAA1099 #2 data               */

/* ══════════════════════════════════════════════════════════════════════
 * WC СИСТЕМНЫЕ ПЕРЕМЕННЫЕ — адреса в ROM-области ядра WC
 * ══════════════════════════════════════════════════════════════════════ */
#define WC_ADDR_PAGES       0x6000  /* [3 байта] страницы #0000,#4000,#8000 */
#define WC_ADDR_ABT         0x6004  /* флаг ESC                             */
#define WC_ADDR_ENT         0x6005  /* флаг ENTER                           */
#define WC_ADDR_TMN         0x6009  /* таймер (инкр. по INT), uint16        */
#define WC_ADDR_CNFV        0x600D  /* версия конфигурации                  */
#define WC_ADDR_HEI         0x600E  /* высота экрана (25/30/36 строк)       */

#define WC_CNFV_PWM         0x00
#define WC_CNFV_3BIT        0x01
#define WC_CNFV_4BIT        0x02
#define WC_CNFV_5BIT        0x03
#define WC_CNFV_VDAC2       0x07

/* ══════════════════════════════════════════════════════════════════════
 * SFR — прямой доступ к портам из SDCC-C
 * ══════════════════════════════════════════════════════════════════════
 *
 * __sfr __at 0xNN      — 8-bit port:  out (N), a  /  in a, (N)
 * __sfr __banked __at 0xNNNN — 16-bit port:  ld bc,#NNNN; out (c),a
 *
 * Не создают переменных/символов — чисто compile-time mapping.
 * Присваивание  sfr_xxx = val;   генерирует inline OUT.
 * Чтение         val = sfr_xxx;   генерирует inline IN.
 * ══════════════════════════════════════════════════════════════════════ */

/* --- Стандартный ZX Spectrum: 8-bit порт ----------------------------- */
__sfr __at 0xFE                 sfr_zx_fe;      /* border / keyboard     */

/* --- TSConfig регистры (addr = reg<<8 | 0xAF) ----------------------- */
__sfr __banked __at 0x00AF      sfr_vconfig;     /* VConfig               */
__sfr __banked __at 0x01AF      sfr_vpage;       /* VPage                 */
__sfr __banked __at 0x02AF      sfr_gxoffsl;     /* GXOffsL               */
__sfr __banked __at 0x03AF      sfr_gxoffsh;     /* GXOffsH               */
__sfr __banked __at 0x04AF      sfr_gyoffsl;     /* GYOffsL               */
__sfr __banked __at 0x05AF      sfr_gyoffsh;     /* GYOffsH               */
__sfr __banked __at 0x06AF      sfr_tsconfig;    /* TSConfig              */
__sfr __banked __at 0x07AF      sfr_palsel;      /* PalSel                */
__sfr __banked __at 0x0FAF      sfr_border;      /* Border Color          */
__sfr __banked __at 0x10AF      sfr_page0;       /* Page0 (#0000)         */
__sfr __banked __at 0x11AF      sfr_page1;       /* Page1 (#4000)         */
__sfr __banked __at 0x12AF      sfr_page2;       /* Page2 (#8000)         */
__sfr __banked __at 0x13AF      sfr_page3;       /* Page3 (#C000)         */
__sfr __banked __at 0x14AF      sfr_fmaddr;      /* FMAddr                */
__sfr __banked __at 0x15AF      sfr_tmpage;      /* TMPage                */
__sfr __banked __at 0x16AF      sfr_t0gpage;     /* T0GPage               */
__sfr __banked __at 0x17AF      sfr_t1gpage;     /* T1GPage               */
__sfr __banked __at 0x18AF      sfr_sgpage;      /* SGPage                */
__sfr __banked __at 0x20AF      sfr_sysconfig;   /* SysConfig (CPU)       */
__sfr __banked __at 0x21AF      sfr_memconfig;   /* MemConfig             */
__sfr __banked __at 0x22AF      sfr_hsint;       /* HSINT                 */
__sfr __banked __at 0x23AF      sfr_vsintl;      /* VSINTL                */
__sfr __banked __at 0x24AF      sfr_vsinth;      /* VSINTH                */
__sfr __banked __at 0x2AAF      sfr_intmask;     /* INTMask               */
__sfr __banked __at 0x2BAF      sfr_cacheconfig;  /* CacheConfig          */
__sfr __banked __at 0x29AF      sfr_fddvirt;     /* FDDVirt               */

/* --- DMA --- */
__sfr __banked __at 0x1AAF      sfr_dmasaddrh;   /* DMA Src Addr H        */
__sfr __banked __at 0x1BAF      sfr_dmasaddrx;   /* DMA Src Addr X        */
__sfr __banked __at 0x19AF      sfr_dmasaddrl;   /* DMA Src Addr L        */
__sfr __banked __at 0x1CAF      sfr_dmadaddrl;   /* DMA Dst Addr L        */
__sfr __banked __at 0x1DAF      sfr_dmadaddrh;   /* DMA Dst Addr H        */
__sfr __banked __at 0x1EAF      sfr_dmadaddrx;   /* DMA Dst Addr X        */
__sfr __banked __at 0x26AF      sfr_dmalen;      /* DMA Burst Length      */
__sfr __banked __at 0x27AF      sfr_dmactrl;     /* DMA Ctrl / Status     */
__sfr __banked __at 0x28AF      sfr_dmanum;      /* DMA Bursts Num L      */
__sfr __banked __at 0x2CAF      sfr_dmanumh;     /* DMA Bursts Num H      */

/* --- OPL3 (YMF262) --- */
__sfr __banked __at 0x00C4      sfr_opl3_addr0;  /* Bank0 address         */
__sfr __banked __at 0x00C5      sfr_opl3_data0;  /* Bank0 data            */
__sfr __banked __at 0x00C6      sfr_opl3_addr1;  /* Bank1 address         */
__sfr __banked __at 0x00C7      sfr_opl3_data1;  /* Bank1 data            */

/* --- AY-3-8910 / YM2149 --- */
__sfr __banked __at 0xFFFD      sfr_ay_addr;     /* reg select / read     */
__sfr __banked __at 0xBFFD      sfr_ay_data;     /* data write            */

/* --- SAA1099 --- */
__sfr __banked __at 0x01FF      sfr_saa_addr;    /* SAA1099 #1 address    */
__sfr __banked __at 0x00FF      sfr_saa_data;    /* SAA1099 #1 data       */
__sfr __banked __at 0x01FE      sfr_saa2_addr;   /* SAA1099 #2 address    */
__sfr __banked __at 0x00FE      sfr_saa2_data;   /* SAA1099 #2 data       */

/* ══════════════════════════════════════════════════════════════════════
 * ФУНКЦИИ — обёртки
 * ══════════════════════════════════════════════════════════════════════ */

/** Запись значения в произвольный TSConfig-порт (asm, для динамических адресов)
 *  @param port  TSCONF_PORT(reg)
 *  @param val   значение */
extern void tsconf_out(uint16_t port, uint8_t val);

/* ── Удобные макросы (через __sfr — inline, без call) ────────────────── */

/** Установить скорость CPU (TSCONF_CPU_3_5MHZ / 7MHZ / 14MHZ) */
#define tsconf_set_cpu(speed) \
    (sfr_sysconfig = (uint8_t)((speed) & TSCONF_CPU_MASK))

/** Установить скорость CPU с управлением кэшем */
#define tsconf_set_cpu_cache(speed, cache_en) \
    (sfr_sysconfig = (uint8_t)(((speed) & TSCONF_CPU_MASK) | ((cache_en) ? TSCONF_CACHE_EN : 0)))

/** Установить маску прерываний (OR нужных TSCONF_INT_*) */
#define tsconf_set_intmask(mask) \
    (sfr_intmask = (uint8_t)(mask))

/** Настроить позицию FRAME-INT: line=0–319, hpos=0–223 */
#define tsconf_set_frame_int(line, hpos) do { \
    sfr_vsinth = (uint8_t)(((line) >> 8) & 0x01); \
    sfr_vsintl = (uint8_t)((line) & 0xFF);        \
    sfr_hsint  = (uint8_t)(hpos);                  \
} while(0)

/** Выключить FRAME-INT полностью */
#define tsconf_disable_frame_int() \
    (sfr_hsint = TSCONF_HSINT_OFF)

/** Настроить кэш по окнам: маска из TSCONF_CACHE_EN_* */
#define tsconf_set_cache(mask) \
    (sfr_cacheconfig = (uint8_t)(mask))

/** Установить цвет бордюра (TSConfig-регистр) */
#define tsconf_set_border(color) \
    (sfr_border = (uint8_t)(color))

/** Подключить видеостраницу (номер 0–63) на адрес #C000 */
#define tsconf_set_vpage(page) \
    (sfr_vpage = (uint8_t)(page))

/** Установить видеорежим (OR из TSCONF_VM_*, TSCONF_RRES_*, TSCONF_VC_*) */
#define tsconf_set_vconfig(cfg) \
    (sfr_vconfig = (uint8_t)(cfg))

/** Подключить RAM-страницу page на окно #C000 (Page3) */
#define tsconf_set_page3(page) \
    (sfr_page3 = (uint8_t)(page))

/** Подключить RAM-страницу page на окно #8000 (Page2) */
#define tsconf_set_page2(page) \
    (sfr_page2 = (uint8_t)(page))

#endif /* TSCONF_H */

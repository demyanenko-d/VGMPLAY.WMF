# VGM Player for OPL3 - Architecture Document

## Overview
Professional VGM player plugin for Wild Commander on TSConfig platform, designed for sample-accurate playback of VGM files on YMF262 (OPL3) hardware.

## System Architecture

### Platform: TSConfig ZX Spectrum Clone
- **CPU**: Z80 @ 7/14/28 MHz (switchable)
- **Video**: 320 lines, 15625 Hz line frequency
- **OPL3**: YMF262 @ 14.31818 MHz on ports #C4-#C7
- **Memory**: Up to 4MB RAM, paging system

### Timing Strategy

#### 1. CPU @ 7 MHz (No-Wait Mode)
For maximum timing precision, plugin runs at 7 MHz with no wait states:
- **1 sample @ 44.1 kHz** = 22.676 µs = ~159 CPU cycles
- **1 frame @ 50 Hz** = 20 ms = 882 samples = 140,000 cycles
- Port #01AF bits 6-7 = 00 for 7 MHz mode

#### 2. Line Interrupt Timing (TSConfig Extension)
TSConfig provides line interrupts for microsecond-precision delays:
- **320 lines per frame** @ 50 Hz
- **Line frequency**: 15625 Hz
- **Time per line**: 64 µs (~448 CPU cycles @ 7 MHz)
- **Precision**: 64 µs granularity
- **Register**: Port #00AF (line position), Port #01AF bit 0 (enable)

#### 3. OPL3 Hardware Delays
YMF262 requires specific delays between register accesses:
- **After address write**: 32 OPL clock cycles = 2.23 µs ≈ 16 CPU cycles @ 7 MHz
- **After data write**: 84 OPL clock cycles = 5.87 µs ≈ 41 CPU cycles @ 7 MHz
- Implemented via cycle-counted NOP loops

### Modular Architecture

```
src/
├── vgmplayer.asm    - Main plugin entry point
├── wcapi.asm        - Wild Commander API wrappers
├── timing.asm       - Precision timing (line interrupts + cycles)
├── opl3.asm         - YMF262 hardware driver
├── vgm.asm          - VGM format parser (header + GD3 tags)
├── playback.asm     - VGM command processor
└── ui.asm           - User interface (Wild Player style)
```

### VGM Command Support

Based on analysis of 1,012 VGM files (see statistics), implemented commands:

| Opcode | Command | Files % | Notes |
|--------|---------|---------|-------|
| 0x5E | YMF262 Port 0 | 99.01% | Primary OPL3 register writes |
| 0x5F | YMF262 Port 1 | 99.01% | Secondary OPL3 bank |
| 0x66 | END | 99.01% | End of data marker |
| 0x61 | WAIT_NN | 98.02% | 16-bit sample delay |
| 0x62 | WAIT_60HZ | 40.81% | 735 samples (1/60 sec) |
| 0x63 | WAIT_50HZ | 42.09% | 882 samples (1/50 sec) |
| 0x70-0x7F | WAIT_1-16 | 34.68-23.02% | Short delays (1-16 samples) |

### UI Update Strategy

**Problem**: UI updates cause audio glitches due to CPU time consumption.

**Solution**: Conditional UI updates only during large pauses:
- **Threshold**: 50 ms (2205 samples @ 44.1 kHz)
- **During waits < 50ms**: No UI update, just accumulate samples
- **During waits >= 50ms**: Safe to update display
- **UI components**: Time, pause status, metadata (buffered)

### Memory Layout

```
#0000-#3FFF : System RAM / Stack
#4000-#xxxx : VGM file data (entire file loaded, up to 3MB)
#C000-#FFFF : Plugin code + work buffers (16KB pages)
```

### VGM File Loading

1. **Check file size** (max 3MB = 3*1024*1024 = #300000 bytes)
2. **Load entire file** into memory starting at #4000
3. **Parse header** (128+ bytes, v1.50+ format)
4. **Extract metadata** from GD3 tag (UTF-16LE strings)
5. **Validate OPL3 clock** (offset #5C must be non-zero)

### Playback Engine

**Main Loop** (no interrupts - cycle-driven):
```
MAIN_LOOP:
    PROCESS_VGM_FRAME()         ; Process 882 samples worth of commands
    UPDATE_DISPLAY_CONDITIONAL() ; UI update if large pause
    CHECK_KEYS()                ; Input handling
    if song_ended:
        if loop_offset: restart_loop()
        else: next_track()
    repeat
```

**VGM Command Processing**:
```
while frame_samples < 882:
    command = GET_VGM_BYTE()
    switch (command):
        case 0x5E: OPL3_WRITE_PORT0(); break
        case 0x5F: OPL3_WRITE_PORT1(); break
        case 0x61: WAIT_NN(); break
        case 0x62: WAIT_60HZ(); break
        case 0x63: WAIT_50HZ(); break
        case 0x70-0x7F: WAIT_SHORT(); break
        case 0x66: END(); return
    if wait > UI_THRESHOLD: DELAY_SAMPLES()
```

### User Interface (WC API)

Wild Commander provides window management API (entry point #6006):

**Window Descriptor** (IX register):
```
+0: Flags (4=border, 16=shadow, 128=visible, 64=cursor)
+2-3: X, Y position
+4-5: Width, Height
+6: Color (PAPER+INK)
+8-9: Buffer pointer (auto-allocated by WC)
+10-11: Line info
+12+: Title text pointer
```

**API Functions**:
- `A=1`: PRWOW - Draw window
- `A=2`: RRESB - Remove window
- `A=3`: PRSRW - Print string in window
- `A=11`: TXTPR - Print formatted text
- `A=16/22/23`: SPKE/ENKE/ESC - Check keys
- `A=48`: LOAD512 - Load 512-byte blocks

**Layout** (similar to Wild Player):
```
┌─────────────────────────────────────────────────────────┐
│ VGM Player for OPL3                                     │
├─────────────────────────────────────────────────────────┤
│ File: track.vgm                                         │
│                                                         │
│ Game:   Super Game Plus                                │
│ Track:  Level 1 Theme                                   │
│ Author: Composer Name                                   │
│ System: Arcade                                          │
│                                                         │
│ Status: Playing / PAUSED                                │
│ Time:   02:35.00                                        │
│                                                         │
│ SPACE - Next  M - Pause  ESC - Exit                    │
└─────────────────────────────────────────────────────────┘
```

### Error Handling

**Error Codes**:
1. File too large (>3MB)
2. File load failed
3. Invalid VGM format (bad signature)
4. No OPL3 data (clock = 0 at offset #5C)

**Display**: Error window with message, wait for ESC

### Controls

- **ESC**: Exit player
- **SPACE**: Skip to next track
- **ENTER**: Play next file in directory (future)
- **M**: Pause/unpause
- **F1**: Help window (future)

### Performance Considerations

**CPU Usage** @ 7 MHz:
- OPL3 register write: ~60 cycles (with delays)
- VGM byte read: ~20 cycles
- Command dispatch: ~30 cycles
- Total per command: ~110 cycles avg.
- Commands per frame: ~100-200 typical
- Frame overhead: 11,000-22,000 cycles (7.8-15.7% @ 140k/frame)
- **Remaining CPU**: 84-93% available for timing/UI

**Memory Usage**:
- Plugin code: ~8-12 KB
- VGM file: up to 3 MB
- Work buffers: ~2 KB
- Total: ~3 MB max

### Build System

**Assembler**: sjasm
**Command**: `sjasm vgmplayer.asm vgmplay.wmf`
**Output**: `vgmplay.wmf` (Wild Commander Module File)

### Future Enhancements

1. **VGZ support** (gzipped VGM) - decompression on load
2. **Playlist** - multiple file playback
3. **Visualizer** - channel activity display
4. **Equalizer** - OPL3 volume controls
5. **Export** - save to different format
6. **32-bit addressing** - proper >64KB file support

## Technical References

- **TSConfig specification**: TSConfRef/tsconf_en.md
- **Wild Commander API**: doc/03_WC_API_Reference.md
- **VGM format**: doc/05_VGM_Format.md
- **OPL3 datasheet**: YMF262 Application Manual
- **Z80 timing**: Z80 CPU User Manual

## Version History

- **v1.0** (2026-03-19): Initial release
  - Full VGM 1.50+ support
  - OPL3 hardware playback
  - 7 MHz cycle-accurate timing
  - Line interrupt timing library
  - Professional UI (Wild Player style)
  - GD3 metadata display
  - Pause/resume/skip controls

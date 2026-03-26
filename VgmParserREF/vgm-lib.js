/**
 * VGM Library v1.71 Specification Compliant Parser
 * 
 * Based on VGM Specification v1.71 beta by Valley Bell
 * https://vgmrips.net/misc/vgmspec171.txt
 * 
 * "VGM (Video Game Music) is a sample-accurate sound logging format for the Sega
 * Master System, the Sega Game Gear and possibly many other machines (e.g. Sega
 * Genesis).
 * 
 * The normal file extension is .vgm but files can also be GZip compressed into
 * .vgz files. However, a VGM player should attempt to support compressed and
 * uncompressed files with either extension."
 */

const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

/**
 * VGM Header Chip Clock Offsets (VGM Specification v1.71)
 * 
 * "The format starts with a 256 byte header"
 * "All integer values are *unsigned* and written in "Intel" byte order (Little
 * Endian), so for example 0x12345678 is written as 0x78 0x56 0x34 0x12."
 * 
 * Офсеты тактовых частот чипов в заголовке VGM файла согласно спецификации v1.71.
 * Каждый чип имеет 4-байтовое поле с тактовой частотой в Гц.
 * "It should be 0 if there is no [chip] used."
 */
const CHIP_OFFSETS = {
    // "0x0C: SN76489 clock (32 bits)
    //         Input clock rate in Hz for the SN76489 PSG chip. A typical value is
    //         3579545. It should be 0 if there is no PSG chip used.
    //         Note: Bit 31 (0x80000000) is used on combination with the dual-chip-bit
    //         to indicate that this is a T6W28. (PSG variant used in NeoGeo Pocket)"
    SN76489: { offset: 0x0C, name: 'SN76489 PSG', minVersion: 0x100 },
    
    // "0x10: YM2413 clock (32 bits)
    //         Input clock rate in Hz for the YM2413 chip. A typical value is 3579545.
    //         It should be 0 if there is no YM2413 chip used."
    YM2413: { offset: 0x10, name: 'YM2413 OPLL', minVersion: 0x100 },
    // "0x14: GD3 offset (32 bits)
    //         Relative offset to GD3 tag. 0 if no GD3 tag.
    //         GD3 tags are descriptive tags similar in use to ID3 tags in MP3 files."
    GD3: { offset: 0x14, name: 'GD3 offset', minVersion: 0x100 },
    
    // "0x2C: YM2612 clock (32 bits)
    //         Input clock rate in Hz for the YM2612 chip. A typical value is 7670454.
    //         For version 1.01 and earlier files, the YM2413 clock rate should be
    //         used for the clock rate of the YM2612."
    YM2612: { offset: 0x2C, name: 'YM2612 OPN2', minVersion: 0x150 },
    
    // "0x30: YM2151 clock (32 bits)
    //         Input clock rate in Hz for the YM2151 chip. A typical value is 3579545."
    // "0x30: YM2151 clock (32 bits)
    //         Input clock rate in Hz for the YM2151 chip. A typical value is 3579545.
    //         It should be 0 if there us no YM2151 chip used."
    YM2151: { offset: 0x30, name: 'YM2151 OPM', minVersion: 0x150 },
    // "[VGM 1.51 additions:]
    // 0x38: Sega PCM clock (32 bits)
    //         Input clock rate in Hz for the Sega PCM chip. A typical value is
    //         4000000. It should be 0 if there is no Sega PCM chip used."
    SegaPCM: { offset: 0x38, name: 'Sega PCM', minVersion: 0x151 },
    // "0x40: RF5C68 clock (32 bits)
    //         Input clock rate in Hz for the RF5C68 PCM chip. A typical value is
    //         12500000. It should be 0 if there is no RF5C68 chip used."
    RF5C68: { offset: 0x40, name: 'RF5C68 PCM', minVersion: 0x151 },
    // "0x44: YM2203 clock (32 bits)
    //         Input clock rate in Hz for the YM2203 chip. A typical value is 3000000."
    YM2203: { offset: 0x44, name: 'YM2203 OPN', minVersion: 0x151 },
    // "0x48: YM2608 clock (32 bits)
    //         Input clock rate in Hz for the YM2608 chip. A typical value is 8000000."
    YM2608: { offset: 0x48, name: 'YM2608 OPNA', minVersion: 0x151 },
    // "0x4C: YM2610/YM2610B clock (32 bits)
    //         Input clock rate in Hz for the YM2610/B chip. A typical value is
    //         8000000. It should be 0 if there is no YM2610/B chip used.
    //         Note: Bit 31 is used to set whether it is a YM2610 or a YM2610B chip.
    //         If bit 31 is set it is an YM2610B, if bit 31 is clear it is an YM2610."
    YM2610: { offset: 0x4C, name: 'YM2610/B OPNB', minVersion: 0x151 },
    // "0x50: YM3812 clock (32 bits)
    //         Input clock rate in Hz for the YM3812 chip. A typical value is 3579545."
    YM3812: { offset: 0x50, name: 'YM3812 OPL2', minVersion: 0x151 },
    // "0x54: YM3526 clock (32 bits)
    //         Input clock rate in Hz for the YM3526 chip. A typical value is 3579545."
    YM3526: { offset: 0x54, name: 'YM3526 OPL', minVersion: 0x151 },
    // "0x58: Y8950 clock (32 bits)
    //         Input clock rate in Hz for the Y8950 chip. A typical value is 3579545."
    Y8950: { offset: 0x58, name: 'Y8950 OPL', minVersion: 0x151 },
    // "0x5C: YMF262 clock (32 bits)
    //         Input clock rate in Hz for the YMF262 chip. A typical value is 14318180."
    YMF262: { offset: 0x5C, name: 'YMF262 OPL3', minVersion: 0x151 },
    // "0x60: YMF278B clock (32 bits)
    //         Input clock rate in Hz for the YMF278B chip. A typical value is
    //         33868800. It should be 0 if there is no YMF278B chip used."
    YMF278B: { offset: 0x60, name: 'YMF278B OPL4', minVersion: 0x151 },
    // "0x64: YMF271 clock (32 bits)
    //         Input clock rate in Hz for the YMF271 chip. A typical value is
    //         16934400. It should be 0 if there is no YMF271 chip used."
    YMF271: { offset: 0x64, name: 'YMF271 OPX', minVersion: 0x151 },
    // "0x68: YMZ280B clock (32 bits)
    //         Input clock rate in Hz for the YMZ280B chip. A typical value is
    //         16934400. It should be 0 if there is no YMZ280B chip used."
    YMZ280B: { offset: 0x68, name: 'YMZ280B PCMD8', minVersion: 0x151 },
    // "0x6C: RF5C164 clock (32 bits)
    //         Input clock rate in Hz for the RF5C164 PCM chip. A typical value is
    //         12500000. It should be 0 if there is no RF5C164 chip used."
    RF5C164: { offset: 0x6C, name: 'RF5C164 PCM', minVersion: 0x151 },
    // "0x70: PWM clock (32 bits)
    //         Input clock rate in Hz for the PWM chip. A typical value is
    //         23011361. It should be 0 if there is no PWM chip used."
    PWM: { offset: 0x70, name: 'PWM', minVersion: 0x151 },
    // "0x74: AY8910 clock (32 bits)
    //         Input clock rate in Hz for the AY8910 chip. A typical value is 1789750.
    //         It should be 0 if there is no AY8910 chip used."
    AY8910: { offset: 0x74, name: 'AY8910 PSG', minVersion: 0x151 },
    // "[VGM 1.61 additions:]
    // 0x80: GameBoy DMG clock (32 bits)
    //         Input clock rate in Hz for the GameBoy DMG chip, LR35902. A typical
    //         value is 4194304. It should be 0 if there is no GB DMG chip used."
    DMG: { offset: 0x80, name: 'GameBoy DMG', minVersion: 0x161 },
    // "0x84: NES APU clock (32 bits)
    //         Input clock rate in Hz for the NES APU chip, N2A03. A typical value is
    //         1789772. It should be 0 if there is no NES APU chip used.
    //         Note: Bit 31 is used to enable the FDS sound addon. Set to enable,
    //         clear to disable."
    NES_APU: { offset: 0x84, name: 'NES APU', minVersion: 0x161 },
    // "0x88: MultiPCM clock (32 bits)
    //         Input clock rate in Hz for the MultiPCM chip. A typical value is
    //         8053975. It should be 0 if there is no MultiPCM chip used."
    MultiPCM: { offset: 0x88, name: 'MultiPCM', minVersion: 0x161 },
    // "0x8C: uPD7759 clock (32 bits)
    //         Input clock rate in Hz for the uPD7759 chip. A typical value is 640000.
    //         It should be 0 if there is no uPD7759 chip used."
    uPD7759: { offset: 0x8C, name: 'uPD7759 ADPCM', minVersion: 0x161 },
    // "0x90: OKIM6258 clock (32 bits)
    //         Input clock rate in Hz for the OKIM6258 chip. A typical value is
    //         4000000. It should be 0 if there is no OKIM6258 chip used."
    OKIM6258: { offset: 0x90, name: 'OKIM6258 ADPCM', minVersion: 0x161 },
    // "0x98: OKIM6295 clock (32 bits)
    //         Input clock rate in Hz for the OKIM6295 chip. A typical value is
    //         8000000. It should be 0 if there is no OKIM6295 chip used."
    OKIM6295: { offset: 0x98, name: 'OKIM6295 ADPCM', minVersion: 0x161 },
    // "0x9C: K051649 clock (32 bits)
    //         Input clock rate in Hz for the K051649 chip. A typical value is
    //         1500000. It should be 0 if there is no K051649 chip used."
    K051649: { offset: 0x9C, name: 'K051649 SCC', minVersion: 0x161 },
    // "0xA0: K054539 clock (32 bits)
    //         Input clock rate in Hz for the K054539 chip. A typical value is
    //         18432000. It should be 0 if there is no K054539 chip used."
    K054539: { offset: 0xA0, name: 'K054539 PCM', minVersion: 0x161 },
    // "0xA4: HuC6280 clock (32 bits)
    //         Input clock rate in Hz for the HuC6280 chip. A typical value is
    //         3579545. It should be 0 if there is no HuC6280 chip used."
    HuC6280: { offset: 0xA4, name: 'HuC6280 PSG', minVersion: 0x161 },
    // "0xA8: C140 clock (32 bits)
    //         Input clock rate in Hz for the C140 chip. A typical value is 8000000.
    //         It should be 0 if there is no C140 chip used."
    C140: { offset: 0xA8, name: 'C140', minVersion: 0x161 },
    // "0xAC: K053260 clock (32 bits)
    //         Input clock rate in Hz for the K053260 chip. A typical value is 
    //         3579545. It should be 0 if there is no K053260 chip used."
    K053260: { offset: 0xAC, name: 'K053260 PCMA', minVersion: 0x161 },
    // "0xB0: Pokey clock (32 bits)
    //         Input clock rate in Hz for the Pokey chip. A typical value is 1789772.
    //         It should be 0 if there is no Pokey chip used."
    Pokey: { offset: 0xB0, name: 'Pokey', minVersion: 0x161 },
    // "0xB4: QSound clock (32 bits)
    //         Input clock rate in Hz for the QSound chip. A typical value is 4000000.
    //         It should be 0 if there is no QSound chip used."
    QSound: { offset: 0xB4, name: 'QSound', minVersion: 0x161 },
    // "[VGM 1.71 additions:]
    // 0xB8: SCSP clock (32 bits)
    //         Input clock rate in Hz for the SCSP chip. A typical value is 22579200.
    //         It should be 0 if there is no SCSP chip used."
    SCSP: { offset: 0xB8, name: 'SCSP', minVersion: 0x171 },
    // "0xC0: WonderSwan clock (32 bits)
    //         Input clock rate in Hz for the WonderSwan chip. A typical value is
    //         3072000. It should be 0 if there is no WonderSwan chip used."
    WonderSwan: { offset: 0xC0, name: 'WonderSwan', minVersion: 0x171 },
    // "0xC4: Virtual Boy VSU clock (32 bits)
    //         Input clock rate in Hz for the VSU chip. A typical value is 5000000.
    //         It should be 0 if there is no VSU chip used."
    VSU: { offset: 0xC4, name: 'Virtual Boy VSU', minVersion: 0x171 },
    // "0xC8: SAA1099 clock (32 bits) 
    //         Input clock rate in Hz for the SAA1099 chip. A typical value is
    //         8000000. It should be 0 if there is no SAA1099 chip used."
    // [VGM 1.71 additions] - Creative Music System sound chip
    SAA1099: { offset: 0xC8, name: 'SAA1099', minVersion: 0x171 },
    // "0xCC: ES5503 clock (32 bits)
    //         Input clock rate in Hz for the ES5503 chip. A typical value is 7159090.
    //         It should be 0 if there is no ES5503 chip used."
    ES5503: { offset: 0xCC, name: 'ES5503 DOC', minVersion: 0x171 },
    // "0xD0: ES5505/ES5506 clock (32 bits)
    //         Input clock rate in Hz for the ES5506 chip. A typical value is
    //         16000000. It should be 0 if there is no ES5506 chip used.
    //         Note: Bit 31 is used to set whether it is an ES5505 or an ES5506 chip.
    //         If bit 31 is set it is an ES5506, if bit 31 is clear it is an ES5505."
    ES5505: { offset: 0xD0, name: 'ES5505/ES5506 OTTO', minVersion: 0x171 },
    // "0xD8: Seta X1-010 clock (32 bits)
    //         Input clock rate in Hz for the X1-010 chip. A typical value is
    //         16000000. It should be 0 if there is no X1-010 chip used."
    X1_010: { offset: 0xD8, name: 'X1-010 Seta', minVersion: 0x171 },
    // "0xDC: Namco C352 clock (32 bits)
    //         Input clock rate in Hz for the C352 chip. A typical value is 24192000.
    //         It should be 0 if there is no C352 chip used."
    C352: { offset: 0xDC, name: 'C352', minVersion: 0x171 },
    // "0xE0: Irem GA20 clock (32 bits)
    //         Input clock rate in Hz for the GA20 chip. A typical value is 3579545.
    //         It should be 0 if there is no GA20 chip used."
    GA20: { offset: 0xE0, name: 'GA20', minVersion: 0x171 }
};

/**
 * VGM Commands (VGM Specification v1.71)
 * 
 * "Starting at the location specified by the VGM data offset (or,
 * offset 0x40 for file versions below 1.50) is found a sequence of commands
 * containing data written to the chips or timing information. A command is one of:"
 * 
 * "Some ranges are reserved for future use, with different numbers of operands:"
 * "On encountering these, the correct number of bytes should be skipped."
 */
// VGM команды (опкоды)
const VGM_COMMANDS = {
    // "0x4F dd    : Game Gear PSG stereo, write dd to port 0x06"
    0x4F: { name: 'GG_PSG_STEREO', params: 1, description: 'Game Gear PSG stereo' },
    // "0x50 dd    : PSG (SN76489/SN76496) write value dd"
    0x50: { name: 'PSG_WRITE', params: 1, description: 'PSG (SN76489/SN76496) write value' },
    
    // "0x51 aa dd : YM2413, write value dd to register aa"
    0x51: { name: 'YM2413_WRITE', params: 2, description: 'YM2413 write' },
    
    // "0x52 aa dd : YM2612 port 0, write value dd to register aa"
    0x52: { name: 'YM2612_PORT0', params: 2, description: 'YM2612 port 0 write' },
    // "0x53 aa dd : YM2612 port 1, write value dd to register aa"
    0x53: { name: 'YM2612_PORT1', params: 2, description: 'YM2612 port 1 write' },
    
    // "0x54 aa dd : YM2151, write value dd to register aa"
    0x54: { name: 'YM2151_WRITE', params: 2, description: 'YM2151 write' },
    
    // "0x55 aa dd : YM2203, write value dd to register aa"
    0x55: { name: 'YM2203_WRITE', params: 2, description: 'YM2203 write' },
    
    // "0x56 aa dd : YM2608 port 0, write value dd to register aa"
    0x56: { name: 'YM2608_PORT0', params: 2, description: 'YM2608 port 0 write' },
    // "0x57 aa dd : YM2608 port 1, write value dd to register aa"
    0x57: { name: 'YM2608_PORT1', params: 2, description: 'YM2608 port 1 write' },
    
    // "0x58 aa dd : YM2610 port 0, write value dd to register aa"
    0x58: { name: 'YM2610_PORT0', params: 2, description: 'YM2610 port 0 write' },
    // "0x59 aa dd : YM2610 port 1, write value dd to register aa"
    0x59: { name: 'YM2610_PORT1', params: 2, description: 'YM2610 port 1 write' },
    
    // "0x5A aa dd : YM3812, write value dd to register aa"
    0x5A: { name: 'YM3812_WRITE', params: 2, description: 'YM3812 write' },
    
    // "0x5B aa dd : YM3526, write value dd to register aa"
    0x5B: { name: 'YM3526_WRITE', params: 2, description: 'YM3526 write' },
    
    // "0x5C aa dd : Y8950, write value dd to register aa"
    0x5C: { name: 'Y8950_WRITE', params: 2, description: 'Y8950 write' },
    
    // "0x5D aa dd : YMZ280B, write value dd to register aa"
    0x5D: { name: 'YMZ280B_WRITE', params: 2, description: 'YMZ280B write' },
    
    // "0x5E aa dd : YMF262 port 0, write value dd to register aa"
    0x5E: { name: 'YMF262_PORT0', params: 2, description: 'YMF262 port 0 write' },
    // "0x5F aa dd : YMF262 port 1, write value dd to register aa"
    0x5F: { name: 'YMF262_PORT1', params: 2, description: 'YMF262 port 1 write' },
    
    // "0x61 nn nn : Wait n samples, n can range from 0 to 65535 (approx 1.49
    //              seconds). Longer pauses than this are represented by multiple
    //              wait commands."
    0x61: { name: 'WAIT_NNNN', params: 2, description: 'Wait n samples (16-bit)' },
    // "0x62       : wait 735 samples (60th of a second), a shortcut for
    //               0x61 0xdf 0x02"
    0x62: { name: 'WAIT_60HZ', params: 0, description: 'Wait 735 samples (60 Hz)' },
    // "0x63       : wait 882 samples (50th of a second), a shortcut for
    //               0x61 0x72 0x03"
    0x63: { name: 'WAIT_50HZ', params: 0, description: 'Wait 882 samples (50 Hz)' },
    
    // "0x66       : end of sound data"
    0x66: { name: 'END', params: 0, description: 'End of sound data' },
    
    // "0x67 ...   : data block: see below"
    0x67: { name: 'DATA_BLOCK', params: -1, description: 'Data block' },
    
    // "0x68 ...   : PCM RAM write: see below"
    0x68: { name: 'PCM_RAM_WRITE', params: -1, description: 'PCM RAM write' },
    
    // "0x7n       : wait n+1 samples, n can range from 0 to 15."
    // Короткие паузы: 0x70 = 1 sample, 0x71 = 2 samples, ... 0x7F = 16 samples
    ...[...Array(16)].reduce((acc, _, i) => {
        acc[0x70 + i] = { name: `WAIT_${i + 1}`, params: 0, description: `Wait ${i + 1} samples` };
        return acc;
    }, {}),
    
    // "0x8n       : YM2612 port 0 address 2A write from the data bank, then wait 
    //               n samples; n can range from 0 to 15. Note that the wait is n,
    //               NOT n+1. (Note: Written to first chip instance only.)"
    // YM2612 DAC паттерн с ожиданием
    ...[...Array(16)].reduce((acc, _, i) => {
        acc[0x80 + i] = { name: `YM2612_DAC_WAIT_${i}`, params: 0, description: `YM2612 PCM data write & wait ${i} samples` };
        return acc;
    }, {}),
    
    // "0xA0 aa dd : AY8910, write value dd to register aa"
    0xA0: { name: 'AY8910_WRITE', params: 2, description: 'AY8910 write' },
    
    // "0xB0 aa dd : RF5C68, write value dd to register aa"
    0xB0: { name: 'RF5C164_WRITE', params: 2, description: 'RF5C164 write' },
    // "0xB1 aa dd : RF5C164, write value dd to register aa"
    0xB1: { name: 'RF5C68_WRITE', params: 2, description: 'RF5C68 write' },
    // "0xB2 ad dd : PWM, write value ddd to register a (d is MSB, dd is LSB)"
    0xB2: { name: 'PWM_WRITE', params: 2, description: 'PWM write' },
    // "0xB3 aa dd : GameBoy DMG, write value dd to register aa
    //               Note: Register 00 equals GameBoy address FF10."
    0xB3: { name: 'DMG_WRITE', params: 2, description: 'GameBoy DMG write' },
    // "0xB4 aa dd : NES APU, write value dd to register aa"
    0xB4: { name: 'NES_APU_WRITE', params: 2, description: 'NES APU write' },
    // "0xB5 aa dd : MultiPCM, write value dd to register aa"
    0xB5: { name: 'MULTI_PCM_WRITE', params: 2, description: 'MultiPCM write' },
    // "0xB6 aa dd : uPD7759, write value dd to register aa"
    0xB6: { name: 'UPD7759_WRITE', params: 2, description: 'uPD7759 write' },
    // "0xB7 aa dd : OKIM6258, write value dd to register aa"
    0xB7: { name: 'OKIM6258_WRITE', params: 2, description: 'OKIM6258 write' },
    // "0xB8 aa dd : OKIM6295, write value dd to register aa"
    0xB8: { name: 'OKIM6295_WRITE', params: 2, description: 'OKIM6295 write' },
    // "0xB9 aa dd : HuC6280, write value dd to register aa"
    0xB9: { name: 'HUC6280_WRITE', params: 2, description: 'HuC6280 write' },
    // "0xBA aa dd : K053260, write value dd to register aa"
    0xBA: { name: 'K053260_WRITE', params: 2, description: 'K053260 write' },
    // "0xBB aa dd : Pokey, write value dd to register aa"
    0xBB: { name: 'POKEY_WRITE', params: 2, description: 'Pokey write' },
    // "0xBC aa dd : WonderSwan, write value dd to register aa"
    0xBC: { name: 'WONDERSWAN_WRITE', params: 2, description: 'WonderSwan write' },
    // "0xBD aa dd : SAA1099, write value dd to register aa"
    0xBD: { name: 'SAA1099_WRITE', params: 2, description: 'SAA1099 write' },
    // "0xBE aa dd : ES5506, write 8-bit value dd to register aa"
    0xBE: { name: 'ES5506_WRITE', params: 2, description: 'ES5506 write' },
    // "0xBF aa dd : GA20, write value dd to register aa"
    0xBF: { name: 'GA20_WRITE', params: 2, description: 'GA20 write' },
    
    // "0xC0 bbaa dd : Sega PCM, write value dd to memory offset aabb"
    0xC0: { name: 'SEGA_PCM_WRITE', params: 3, description: 'Sega PCM write' },
    // "0xC1 bbaa dd : RF5C68, write value dd to memory offset aabb"
    0xC1: { name: 'RF5C164_MEM_WRITE', params: 3, description: 'RF5C164 memory write' },
    // "0xC2 bbaa dd : RF5C164, write value dd to memory offset aabb"
    0xC2: { name: 'RF5C68_MEM_WRITE', params: 3, description: 'RF5C68 memory write' },
    // "0xC3 cc bbaa : MultiPCM, write set bank offset aabb to channel cc"
    0xC3: { name: 'YM2608_WRITE_PORT2', params: 3, description: 'YM2608 write port 2/3' },
    
    // "0xE0 dddddddd : seek to offset dddddddd (Intel byte order) in PCM data bank"
    0xE0: { name: 'SEEK_PCM', params: 4, description: 'Seek to offset in PCM data bank' },
    
    // "0xE1 aabb ddee: C352 write 16-bit value ddee to register aabb"
    0xE1: { name: 'C352_WRITE', params: 3, description: 'C352 write' }
};

/**
 * Анализирует gzip заголовок VGZ файла согласно RFC 1952
 * 
 * \"The normal file extension is .vgm but files can also be GZip compressed into
 * .vgz files. However, a VGM player should attempt to support compressed and
 * uncompressed files with either extension. (ZLib's GZIO library makes this
 * trivial to implement.)\"
 * 
 * GZIP Заголовок (RFC 1952):
 *   +---+---+---+---+---+---+---+---+---+---+
 *   |ID1|ID2|CM |FLG|     MTIME     |XFL|OS |
 *   +---+---+---+---+---+---+---+---+---+---+
 *
 * \"A gzip file consists of a series of 'members' (compressed data sets).
 *  The format of each member is specified in the following section. The
 *  members simply appear one after another in the file, with no additional
 *  information before, between, or after them.\"
 *
 * ID1 (IDentification 1) = 0x1f
 * ID2 (IDentification 2) = 0x8b  
 * CM  (Compression Method) = обычно 8 (deflate)
 * FLG (FLaGs) = различные флаги
 * MTIME (Modification TIME) = время последней модификации файла
 * XFL (eXtra FLags) = дополнительные флаги компрессии
 * OS  (Operating System) = файловая система/ОС, где создавался файл
 * 
 * @param {Buffer} compressedData - Сжатые gzip данные
 * @returns {Object} Информация о gzip заголовке согласно RFC 1952
 * @throws {Error} При неверной сигнатуре GZIP
 */
function analyzeGZIP(compressedData) {
    const info = {};
    
    // Проверка GZIP magic number согласно RFC 1952:
    // \"The member begins with a fixed 10-byte header, containing a magic number,
    //  a version indicator, a timestamp, extra flags, and an OS indicator.\"
    if (compressedData[0] !== 0x1f || compressedData[1] !== 0x8b) {
        throw new Error('Invalid GZIP signature');
    }
    
    // Compression method (RFC 1952):
    // \"CM (Compression Method) This identifies the compression method used in the file.
    //  CM = 8 denotes the 'deflate' compression method with a window size up to 32K.\"
    const method = compressedData[2];
    const methodNames = {
        0: 'Reserved 0',
        1: 'Reserved 1 (compress)',
        2: 'Reserved 2 (pack)',
        3: 'Reserved 3 (lzh)',
        4: 'Reserved 4',
        5: 'Reserved 5',
        6: 'Reserved 6',
        7: 'Reserved 7',
        8: 'DEFLATE (LZ77 + Huffman)',
        // 9-255 зарезервированы
    };
    info.compressionMethod = methodNames[method] || `Reserved/Unknown (${method})`;
    info.compressionMethodCode = method;
    
    // Флаги гzip заголовка (RFC 1952):
    // "FLG (FLaGs) Reserved FLG bits must be zero. The remaining FLG bits are:
    //  bit 0 FTEXT, bit 1 FHCRC, bit 2 FEXTRA, bit 3 FNAME, bit 4 FCOMMENT"
    const flags = compressedData[3];
    info.flags = {
        FTEXT: !!(flags & 0x01),
        FHCRC: !!(flags & 0x02),
        FEXTRA: !!(flags & 0x04),
        FNAME: !!(flags & 0x08),
        FCOMMENT: !!(flags & 0x10)
    };
    
    // Время модификации файла (RFC 1952):
    // "MTIME (Modification Time) This gives the most recent modification time of the
    //  original file being compressed. The time is in Unix format"
    const mtime = compressedData.readUInt32LE(4);
    info.timestamp = mtime;
    if (mtime > 0) {
        info.modificationDate = new Date(mtime * 1000).toISOString();
    }
    
    // Дополнительные флаги компрессии (RFC 1952):
    // "XFL (eXtra FLags) These may be used by specific compression methods.
    //  The 'deflate' method (CM = 8) sets these flags as follows:
    //    XFL = 2 - compressor used maximum compression, slowest algorithm
    //    XFL = 4 - compressor used fastest algorithm"
    const xfl = compressedData[8];
    const compressionLevels = {
        2: 'Maximum compression (slowest)',
        4: 'Fastest compression'
    };
    info.compressionLevel = compressionLevels[xfl] || 'Default compression';
    info.compressionLevelCode = xfl;
    
    // Операционная система (RFC 1952):
    // "OS (Operating System) This identifies the type of file system on which compression
    //  took place. This may be useful in determining end-of-line convention for text files."
    const os = compressedData[9];
    const osNames = {
        0: 'FAT filesystem (MS-DOS, OS/2, NT/Win32)',
        1: 'Amiga',
        2: 'VMS (or OpenVMS)',
        3: 'Unix',
        4: 'VM/CMS',
        5: 'Atari TOS',
        6: 'HPFS filesystem (OS/2, NT)',
        7: 'Macintosh',
        8: 'Z-System',
        9: 'CP/M',
        10: 'TOPS-20',
        11: 'NTFS filesystem (NT)',
        12: 'QDOS',
        13: 'Acorn RISCOS',
        255: 'Unknown'
    };
    info.os = osNames[os] || `Unknown (${os})`;
    info.osCode = os;
    
    return info;
}

/**
 * Читает VGZ файл и возвращает распакованный VGM + информацию о сжатии
 * 
 * \"The normal file extension is .vgm but files can also be GZip compressed into
 * .vgz files. However, a VGM player should attempt to support compressed and
 * uncompressed files with either extension. (ZLib's GZIO library makes this
 * trivial to implement.)\"
 * 
 * VGZ (VGM GZip) файлы содержат VGM данные, сжатые алгоритмом gzip.
 * Эта функция автоматически определяет формат файла и распаковывает при необходимости.
 * 
 * @param {string} filePath - Путь к VGZ файлу
 * @returns {Object} { data: Buffer с VGM данными, gzipInfo: инфо о сжатии }
 * @throws {Error} При ошибках чтения или распаковки
 */
function readVGZ(filePath) {
    try {
        const compressed = fs.readFileSync(filePath);
        const uncompressed = zlib.gunzipSync(compressed);
        
        // Анализируем gzip заголовок
        const gzipInfo = analyzeGZIP(compressed);
        
        // Добавляем информацию о размерах и степени сжатия
        gzipInfo.compressedSize = compressed.length;
        gzipInfo.uncompressedSize = uncompressed.length;
        gzipInfo.compressionRatio = ((1 - compressed.length / uncompressed.length) * 100).toFixed(2);
        gzipInfo.compressionRatioText = `${gzipInfo.compressionRatio}%`;
        
        return { data: uncompressed, gzipInfo };
    } catch (err) {
        throw new Error(`Failed to read/decompress VGZ: ${err.message}`);
    }
}

/**
 * Парсит заголовок VGM
 */
function parseVGMHeader(vgm) {
    const header = {};
    
    // Проверка сигнатуры
    const signature = vgm.slice(0, 4).toString();
    if (signature !== 'Vgm ') {
        throw new Error('Invalid VGM signature');
    }
    header.signature = signature;
    
    // Версия (формат: 0xAABB где AA - major, BB - minor в BCD)
    const versionRaw = vgm.readUInt32LE(0x08);
    
    // Версионная система VGM использует BCD для minor version
    // Например: 0x0171 = версия 1.71, 0x0161 = версия 1.61
    const major = (versionRaw >> 8) & 0xFF;
    const minorBCD = versionRaw & 0xFF;
    
    // Конвертируем minor version из BCD в десятичную
    const minorHigh = (minorBCD >> 4) & 0x0F;
    const minorLow = minorBCD & 0x0F;
    const minor = minorHigh * 10 + minorLow;
    
    header.version = `${major}.${minor.toString().padStart(2, '0')}`;
    header.versionHex = versionRaw;
    header.versionRaw = versionRaw; // Для отладки
    
    // \"VGM files use a sample rate of 44100 Hz. All wait times are given as a
    //  number of samples to wait at this rate.\"
    // \"VGMs run with a rate of 44100 samples per second. All sample values use
    //  this unit.\"
    // Фиксированная частота дискретизации 44,1 кГц для всех VGM файлов
    header.sampleRate = 44100;
    header.sampleRateNote = 'Fixed by VGM specification';
    
    // "0x18: Total # samples (32 bits)
    //         Total of all wait values in the file."
    header.totalSamples = vgm.readUInt32LE(0x18);
    header.totalSeconds = header.totalSamples / header.sampleRate;
    
    // "0x1C: Loop offset (32 bits)
    //         Relative offset to loop point, or 0 if no loop."
    const loopOffset = vgm.readUInt32LE(0x1C);
    header.loopOffset = loopOffset > 0 ? loopOffset + 0x1C : 0;
    
    // "0x20: Loop # samples (32 bits)
    //         Number of samples in one loop, or 0 if there is no loop.
    //         Total of all wait values between the loop point and the end of the file."
    header.loopSamples = vgm.readUInt32LE(0x20);
    header.loopSeconds = header.loopSamples / header.sampleRate;
    header.hasLoop = header.loopSamples > 0;
    
    // Длительность введения до начала цикла (расчетное значение):
    // intro_samples = total_samples - loop_samples
    header.introSamples = header.totalSamples - header.loopSamples;
    header.introSeconds = header.introSamples / header.sampleRate;
    
    // "[VGM 1.01 additions:]
    // 0x24: Rate (32 bits)
    //         "Rate" of recording in Hz, used for rate scaling on playback. It is
    //         typically 50 for PAL systems and 60 for NTSC systems."
    header.rate = vgm.readUInt32LE(0x24);
    
    // "[VGM 1.50 additions:]
    // 0x34: VGM data offset (32 bits)
    //         Relative offset to VGM data stream.
    //         If the VGM data starts at absolute offset 0x40, this will contain 
    //         value 0x0000000C. For versions prior to 1.50, it should be 0 and the
    //         VGM data must start at offset 0x40."
    let dataOffset = 0x40; // По умолчанию для версий < 1.50
    if (versionRaw >= 0x150) {
        const vgmDataOffset = vgm.readUInt32LE(0x34);
        if (vgmDataOffset > 0) {
            dataOffset = 0x34 + vgmDataOffset;
        }
    }
    header.dataOffset = dataOffset;
    
    // \"0x14: GD3 offset (32 bits)
    //         Relative offset to GD3 tag. 0 if no GD3 tag.
    //         GD3 tags are descriptive tags similar in use to ID3 tags in MP3 files.\"
    // GD3 содержит метаданные: название трека, игры, автора, систему и дату
    const gd3Offset = vgm.readUInt32LE(0x14);
    header.gd3Offset = gd3Offset > 0 ? gd3Offset + 0x14 : 0;
    
    // Размер заголовка в зависимости от версии VGM
    let headerSize = 0x40; // v1.00-1.01: 64 bytes
    if (versionRaw >= 0x150) {
        // v1.50+: Используем VGM data offset для определения размера
        const vgmDataOffset = vgm.readUInt32LE(0x34);
        if (vgmDataOffset > 0) {
            headerSize = 0x34 + vgmDataOffset;
        } else {
            // Если offset не указан, используем размер по умолчанию для версии
            if (versionRaw >= 0x171) {
                headerSize = 0x100; // v1.71+: 256 bytes
            } else if (versionRaw >= 0x161) {
                headerSize = 0xC0; // v1.61-1.70: 192 bytes  
            } else if (versionRaw >= 0x151) {
                headerSize = 0x80; // v1.51-1.60: 128 bytes
            } else {
                headerSize = 0x80; // v1.50: 128 bytes
            }
        }
    }
    header.headerSize = headerSize;
    header.maxHeaderOffset = headerSize;
    
    // Чипы (с учетом версии VGM)
    header.chips = [];
    for (const [chipKey, chipInfo] of Object.entries(CHIP_OFFSETS)) {
        if (chipKey === 'GD3') continue; // Пропускаем GD3 offset
        
        // Проверяем, доступен ли чип в данной версии VGM
        if (chipInfo.minVersion && versionRaw < chipInfo.minVersion) {
            continue; // Чип недоступен в этой версии
        }
        
        const offset = chipInfo.offset;
        // Проверяем, что offset находится в пределах заголовка
        if (offset + 4 <= headerSize && offset + 4 <= vgm.length) {
            const clockRaw = vgm.readUInt32LE(offset);
            
            // \"Dual Chip Support
            // These chips support two instances of a chip in one vgm:
            // PSG, YM2413, YM2612, YM2151, YM2203, YM2608, YM2610, YM3812, YM3526, Y8950,
            // YMZ280B, YMF262, YMF278B, YMF271, AY8910, GameBoy DMG, NES APU, MultiPCM,
            // uPD7759, OKIM6258, OKIM6295, K051649, K054539, HuC6280, C140, K053260, Pokey,
            // SCSP
            // 
            // Dual chip support is activated by setting bit 30 (0x40000000) in the chip's
            // clock value. (Note: The PSG needs this bit set for T6W28 mode.)\"
            const isDualChip = (clockRaw & 0x40000000) !== 0;
            const actualClock = clockRaw & ~0x40000000; // Убираем dual chip бит
            
            if (actualClock > 0 && clockRaw < 0x80000000) { // Проверяем raw значение на адекватность
                // Список чипов поддерживающих dual mode по официальной спецификации VGM v1.71
                // Note: PSG (SN76489) использует бит 30 для T6W28 mode, не для dual chip
                const dualChipSupported = [
                    'YM2413', 'YM2612', 'YM2151', 'YM2203', 'YM2608', 'YM2610', 
                    'YM3812', 'YM3526', 'Y8950', 'YMZ280B', 'YMF262', 'YMF278B', 
                    'YMF271', 'AY8910', 'DMG', 'NES_APU', 'MultiPCM', 'uPD7759', 
                    'OKIM6258', 'OKIM6295', 'K051649', 'K054539', 'HuC6280', 
                    'C140', 'K053260', 'Pokey', 'SCSP'
                    // SAA1099 не в официальном списке, но поддерживается практически
                ];
                
                // Специальная обработка для PSG (SN76489): бит 30 указывает T6W28 mode
                const isT6W28 = chipKey === 'SN76489' && isDualChip;
                const isPsgDualMode = chipKey === 'SN76489' && isDualChip;
                
                // Форматируем версию добавления чипа (BCD формат)
                let versionAddedStr = '1.00';
                if (chipInfo.minVersion) {
                    const vMajor = (chipInfo.minVersion >> 8) & 0xFF;
                    const vMinorHex = chipInfo.minVersion & 0xFF;
                    const vMinorHigh = (vMinorHex >> 4) & 0x0F;
                    const vMinorLow = vMinorHex & 0x0F;
                    const vMinor = vMinorHigh * 10 + vMinorLow;
                    versionAddedStr = `${vMajor}.${vMinor.toString().padStart(2, '0')}`;
                }
                
                // Определяем название чипа с учетом dual mode и специальных случаев
                let chipDisplayName = chipInfo.name;
                let isDualChipForDisplay = false;
                
                if (chipKey === 'SN76489' && isDualChip) {
                    // PSG: бит 30 означает T6W28 mode (NeoGeo Pocket variant)
                    chipDisplayName = 'T6W28 PSG';
                    isDualChipForDisplay = false;
                } else if (isDualChip && (dualChipSupported.includes(chipKey) || chipKey === 'SAA1099')) {
                    // Официально поддерживаемые dual chip + SAA1099 (практическая поддержка)
                    chipDisplayName = `2x ${chipInfo.name}`;
                    isDualChipForDisplay = true;
                }
                
                header.chips.push({
                    type: chipKey,
                    name: chipDisplayName,
                    clock: actualClock,
                    clockRaw: clockRaw,
                    isDualChip: isDualChipForDisplay,
                    isT6W28: isT6W28,
                    clockMHz: (actualClock / 1000000).toFixed(3),
                    versionAdded: versionAddedStr
                });
            }
        }
    }
    
    return header;
}

/**
 * Парсит GD3 теги (метаданные) из VGM файла
 * 
 * \"GD3 tags are descriptive tags similar in use to ID3 tags in MP3 files.
 * See the GD3 specification for more details. The GD3 tag is usually
 * stored immediately after the VGM data.\"
 * 
 * GD3 тег содержит информацию о треке в UTF-16LE кодировке:
 * - Track title (Eng + Jpn)
 * - Game title (Eng + Jpn) 
 * - System name (Eng + Jpn)
 * - Author (Eng + Jpn)
 * - Release date
 * - Creator/Dumper
 * - Notes
 * 
 * @param {Buffer} vgmBuffer - Буфер VGM файла
 * @param {number} gd3Offset - Смещение GD3 тега
 * @returns {Object|null} Распарсенные GD3 теги или null
 */
function parseGD3(vgmBuffer, gd3Offset) {
    if (!gd3Offset || gd3Offset >= vgmBuffer.length) {
        return null;
    }
    
    try {
        // Проверяем сигнатуру GD3
        const signature = vgmBuffer.slice(gd3Offset, gd3Offset + 4).toString();
        if (signature !== 'Gd3 ') {
            return null;
        }
        
        const version = vgmBuffer.readUInt32LE(gd3Offset + 4);
        const dataLength = vgmBuffer.readUInt32LE(gd3Offset + 8);
        
        let pos = gd3Offset + 12;
        const strings = [];
        
        // Читаем UTF-16LE строки (каждая заканчивается на 0x0000)
        for (let i = 0; i < 10; i++) { // GD3 содержит 10 строк
            let str = '';
            while (pos + 1 < vgmBuffer.length) {
                const char = vgmBuffer.readUInt16LE(pos);
                if (char === 0) break;
                str += String.fromCharCode(char);
                pos += 2;
            }
            strings.push(str);
            pos += 2; // Пропускаем нулевой терминатор
        }
        
        return {
            version: version,
            trackTitle: strings[0] || '',
            trackTitleJapanese: strings[1] || '',
            gameName: strings[2] || '',
            gameNameJapanese: strings[3] || '',
            systemName: strings[4] || '',
            systemNameJapanese: strings[5] || '',
            trackAuthor: strings[6] || '',
            trackAuthorJapanese: strings[7] || '',
            date: strings[8] || '',
            ripper: strings[9] || '',
            notes: strings[10] || ''
        };
    } catch (error) {
        return null;
    }
}

/**
 * Парсит команды VGM (опкоды) согласно VGM спецификации v1.71
 * 
 * \"The VGM data is made up of command bytes, some of which are followed by
 *  data bytes. Commands 0x00-0x4f and 0x50-0xff are used for different groups.\"
 *
 * \"VGM files use a sample rate of 44100 Hz. All wait times are given as a
 *  number of samples to wait at this rate.\"
 *
 * Основные группы команд:
 * 0x50-0x5f: PSG (SN76489) - \"0x50 dd : PSG (SN76489/SN76496) write value dd\"
 * 0x61: Wait - \"0x61 nnnn : Wait nnnn samples\"  
 * 0x62: Wait - \"0x62 : Wait 735 samples (60th of a second)\"
 * 0x63: Wait - \"0x63 : Wait 882 samples (50th of a second)\"
 * 0x66: End - \"0x66 : End of sound data\"
 * 0x70-0x8f: Wait short - \"0x7n : Wait n+1 samples, n can range from 0 to 15\"
 * 0xa0-0xa9: YM2612 - \"0xA0 aa dd : YM2612 port 0 address aa data dd\"
 * 0xb0-0xb8: RF5C68 - \"0xB0 aa dd : RF5C68 register aa data dd\"
 * 0xc0-0xdf: Sega PCM - \"0xCn dddd : Sega PCM memory write\"
 * 
 * @param {Buffer} vgm - VGM данные
 * @param {number} dataOffset - Смещение начала команд в VGM
 * @returns {Object} Объект с проанализированными командами и статистикой
 */
function parseVGMCommands(vgm, dataOffset) {
    const commands = {};
    let position = dataOffset;
    let totalSamples = 0;
    let commandCount = 0;
    let writeCommandCount = 0; // Счетчик команд записи данных в чипы (не WAIT команды)
    let totalCommandBytes = 0; // Общее количество байт всех команд (включая WAIT)
    let lastWritePosition = 0; // Позиция в samples последней команды записи данных в чип
    let maxWriteGap = 0; // Максимальный интервал между командами записи в samples
    const writeGaps = []; // Все интервалы между командами (для статистики)
    
    while (position < vgm.length) {
        const opcode = vgm[position];
        commandCount++;
        
        // Обработка команд
        if (opcode === 0x66) {
            // End of sound data
            const key = opcode;
            if (!commands[key]) {
                commands[key] = {
                    opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
                    name: 'END',
                    description: 'End of sound data',
                    count: 0,
                    samples: 0
                };
            }
            commands[key].count++;
            totalCommandBytes += 1;
            break;
        } else if (opcode === 0x61) {
            // \"0x61 nn nn : Wait n samples, n can range from 0 to 65535 (approx 1.49 seconds)\"
            // Wait n samples - точное ожидание на указанное количество семплов
            const samples = vgm.readUInt16LE(position + 1);
            const key = `0x61_${samples}`;
            
            if (!commands[key]) {
                commands[key] = {
                    opcode: '0x61',
                    name: `WAIT_NN(${samples})`,
                    description: `Wait ${samples} samples`,
                    count: 0,
                    samples: 0,
                    waitValue: samples
                };
            }
            commands[key].count++;
            commands[key].samples += samples;
            totalSamples += samples;
            totalCommandBytes += 3;
            position += 3; // opcode + 2 bytes для samples
        } else if (opcode === 0x62) {
            // "0x62 : Wait 735 samples (60th of a second)"
            // Стандартная пауза для NTSC (60 Гц) систем: 735/44100 = 16.67ms
            if (!commands[opcode]) {
                commands[opcode] = {
                    opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
                    name: 'WAIT_60HZ',
                    description: 'Wait 735 samples (60 Hz)',
                    count: 0,
                    samples: 0
                };
            }
            commands[opcode].count++;
            commands[opcode].samples += 735;
            totalSamples += 735;
            totalCommandBytes += 1;

            position += 1;
        } else if (opcode === 0x63) {
            // "0x63 : Wait 882 samples (50th of a second)"
            // Стандартная пауза для PAL (50 Гц) систем: 882/44100 = 20ms
            if (!commands[opcode]) {
                commands[opcode] = {
                    opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
                    name: 'WAIT_50HZ',
                    description: 'Wait 882 samples (50 Hz)',
                    count: 0,
                    samples: 0
                };
            }
            commands[opcode].count++;
            commands[opcode].samples += 882;
            totalSamples += 882;
            totalCommandBytes += 1;
            position += 1;
        } else if (opcode >= 0x70 && opcode <= 0x7F) {
            // Wait 1-16 samples
            const samples = opcode - 0x6F;
            if (!commands[opcode]) {
                const cmdInfo = VGM_COMMANDS[opcode];
                commands[opcode] = {
                    opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
                    name: cmdInfo.name,
                    description: cmdInfo.description,
                    count: 0,
                    samples: 0
                };
            }
            commands[opcode].count++;
            commands[opcode].samples += samples;
            totalSamples += samples;
            totalCommandBytes += 1;
            position += 1;
        } else if (opcode >= 0x80 && opcode <= 0x8F) {
            // YM2612 PCM data + wait
            const samples = opcode - 0x80;
            if (!commands[opcode]) {
                commands[opcode] = {
                    opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
                    name: `YM2612_DAC_WAIT_${samples}`,
                    description: `YM2612 DAC write + wait ${samples} samples`,
                    count: 0,
                    samples: 0
                };
            }
            commands[opcode].count++;
            commands[opcode].samples += samples;
            totalSamples += samples;
            totalCommandBytes += 1;
            position += 1;
        } else if (opcode === 0x67) {
            // Data block: 0x67 0x66 tt ss ss ss ss (data)
            if (!commands[opcode]) {
                commands[opcode] = {
                    opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
                    name: 'DATA_BLOCK',
                    description: 'Data block',
                    count: 0,
                    samples: 0
                };
            }
            commands[opcode].count++;
            position += 2; // Skip 0x67 0x66
            const blockType = vgm[position++];
            const blockSize = vgm.readUInt32LE(position);
            totalCommandBytes += 2 + 1 + 4 + blockSize; // 0x67 + 0x66 + type + size + data
            position += 4 + blockSize;
        } else if (opcode === 0x68) {
            // PCM RAM write
            if (!commands[opcode]) {
                commands[opcode] = {
                    opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
                    name: 'PCM_RAM_WRITE',
                    description: 'PCM RAM write',
                    count: 0,
                    samples: 0
                };
            }
            commands[opcode].count++;
            totalCommandBytes += 12;
            position += 12; // Fixed size
        } else {
            // Стандартные команды записи
            const cmdInfo = VGM_COMMANDS[opcode];
            if (!commands[opcode]) {
                if (cmdInfo) {
                    commands[opcode] = {
                        opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
                        name: cmdInfo.name,
                        description: cmdInfo.description,
                        count: 0,
                        samples: 0
                    };
                } else {
                    commands[opcode] = {
                        opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
                        name: `UNKNOWN_0x${opcode.toString(16).toUpperCase()}`,
                        description: 'Unknown command',
                        count: 0,
                        samples: 0
                    };
                }
            }
            commands[opcode].count++;
            
            // Отслеживаем команды записи (не WAIT, не END, не DATA_BLOCK)
            const isWriteCommand = opcode !== 0x66 && opcode !== 0x67 && opcode !== 0x68 &&
                opcode !== 0x61 && opcode !== 0x62 && opcode !== 0x63 && 
                (opcode < 0x70 || opcode > 0x7F) && (opcode < 0x80 || opcode > 0x8F);
            
            if (isWriteCommand) {
                writeCommandCount++;
                const gap = totalSamples - lastWritePosition;
                // Игнорируем задержки > 1000 семплов
                if (gap > 0 && gap <= 1000) {
                    writeGaps.push(gap);
                    if (gap > maxWriteGap) {
                        maxWriteGap = gap;
                    }
                }
                lastWritePosition = totalSamples;
            }
            
            if (cmdInfo) {
                if (cmdInfo.params > 0) {
                    totalCommandBytes += 1 + cmdInfo.params;
                    position += 1 + cmdInfo.params;
                } else if (cmdInfo.params === 0) {
                    totalCommandBytes += 1;
                    position += 1;
                } else {
                    // Variable length - skip safely
                    totalCommandBytes += 1;
                    position += 1;
                }
            } else {
                // Unknown command - try to skip
                totalCommandBytes += 1;
                position += 1;
            }
        }
        
        // Защита от бесконечного цикла
        if (commandCount > 10000000) {
            console.warn('Too many commands, stopping parsing');
            break;
        }
    }
    
    // Рассчитываем среднюю задержку
    const avgWriteGap = writeGaps.length > 0 
        ? writeGaps.reduce((sum, gap) => sum + gap, 0) / writeGaps.length 
        : 0;
    
    return {
        commands: Object.values(commands).sort((a, b) => b.count - a.count),
        totalCommands: commandCount,
        totalSamples,
        totalSeconds: totalSamples / 44100,
        totalCommandBytes: totalCommandBytes,
        writeCommands: writeCommandCount,
        maxWriteGap: maxWriteGap,
        avgWriteGap: avgWriteGap,
        maxWriteGapMs: (maxWriteGap / 44100 * 1000).toFixed(2),
        avgWriteGapMs: (avgWriteGap / 44100 * 1000).toFixed(2)
    };
}

/**
 * Полный анализ VGM файла согласно спецификации v1.71
 * 
 * \"Starting at the location specified by the VGM data offset (or,
 * offset 0x40 for file versions below 1.50) is found a sequence of commands
 * containing data written to the chips or timing information.\"
 * 
 * Выполняет комплексный анализ VGM файла:
 * - Парсит заголовок с chip clocks и метаданными
 * - Анализирует команды VGM stream
 * - Подсчитывает статистику: команды, samples, timing
 * - Определяет используемые чипы и их частоты
 * - Извлекает информацию о loop points
 * 
 * @param {Buffer} vgmBuffer - Буфер с VGM данными
 * @returns {Object} Полный анализ файла с header, commands и статистикой
 */
function analyzeVGM(vgmBuffer) {
    const header = parseVGMHeader(vgmBuffer);
    const commandData = parseVGMCommands(vgmBuffer, header.dataOffset);
    
    return {
        header,
        commands: commandData.commands,
        stats: {
            totalCommands: commandData.totalCommands,
            totalSamples: commandData.totalSamples,
            totalCommandBytes: commandData.totalCommandBytes,
            calculatedSeconds: commandData.totalSeconds,
            writeCommands: commandData.writeCommands,
            maxWriteGap: commandData.maxWriteGap,
            avgWriteGap: commandData.avgWriteGap,
            maxWriteGapMs: commandData.maxWriteGapMs,
            avgWriteGapMs: commandData.avgWriteGapMs
        }
    };
}

/**
 * Анализ VGZ файла с автоматической распаковкой
 * 
 * \"The normal file extension is .vgm but files can also be GZip compressed into  
 * .vgz files. However, a VGM player should attempt to support compressed and
 * uncompressed files with either extension.\"
 *
 * VGZ (VGM GZip) файлы содержат стандартные VGM данные, сжатые алгоритмом gzip.
 * Эта функция автоматически распаковывает VGZ и выполняет полный анализ VGM,
 * добавляя дополнительную информацию о сжатии (степень сжатия, размеры, метаданные gzip).
 * 
 * @param {string} filePath - Путь к VGZ файлу
 * @returns {Object} Полный анализ VGM + информация о gzip сжатии
 * @throws {Error} При ошибках чтения, распаковки или анализа
 */
function analyzeVGZFile(filePath) {
    const { data: vgm, gzipInfo } = readVGZ(filePath);
    const analysis = analyzeVGM(vgm);
    
    // Добавляем информацию о сжатии
    analysis.compression = gzipInfo;
    
    return analysis;
}

/**
 * Декомпилятор VGM файлов с полной поддержкой спецификации v1.71
 * 
 * \"Starting at the location specified by the VGM data offset (or,
 * offset 0x40 for file versions below 1.50) is found a sequence of commands
 * containing data written to the chips or timing information.\"
 * 
 * Этот декомпилятор обеспечивает:
 * - Полную информацию о заголовке VGM с chip clocks
 * - Парсинг GD3 метаданных (track titles, game info, etc)
 * - Детекцию loop points и структуры файла
 * - Временные метки для навигации по треку
 * - Статистику команд и длительности
 * - Поддержку dual chip режимов
 * 
 * @param {Buffer} vgmBuffer - Буфер с VGM данными
 * @param {Object} options - Опции декомпиляции
 * @returns {Object} Результат декомпиляции со всей информацией
 */
function decompileVGM(vgmBuffer, options = {}) {
    const {
        showTimeMarks = true,  // Показывать временные метки каждую секунду
        showAllCommands = true, // Показывать ВСЕ команды по умолчанию
        maxCommands = 0,        // Без ограничений по умолчанию
        showStatistics = true,  // Показывать статистику
        showHeader = true       // Показывать полную информацию о заголовке
    } = options;
    
    const header = parseVGMHeader(vgmBuffer);
    
    // Парсим GD3 теги если есть
    let gd3 = null;
    if (header.gd3Offset > 0) {
        gd3 = parseGD3(vgmBuffer, header.gd3Offset);
    }
    
    const decompilation = {
        header: header,
        gd3: gd3,
        commands: [],
        statistics: {
            totalCommands: 0,
            totalSamples: 0,
            totalSeconds: 0,
            writeCommands: 0,
            waitCommands: 0,
            dataBlocks: 0,
            timeMarks: [],
            loopFound: false,
            loopSample: 0
        }
    };
    
    // \"VGM files use a sample rate of 44100 Hz. All wait times are given as a
    //  number of samples to wait at this rate.\"
    // 
    // VGM стандарт использует фиксированную частоту дискретизации 44,1 кГц для всех timing операций
    const SAMPLE_RATE = 44100;
    const dataOffset = header.dataOffset;
    
    let position = dataOffset;
    let currentSamples = 0;
    let lastTimeMarkSeconds = 0;
    let commandIndex = 0;
    let commandNumber = 1; // Номер команды для вывода (начинаем с 1)
    
    // Вычисляем позицию лупа в samples
    const loopSample = header.totalSamples - header.loopSamples;
    if (header.hasLoop && header.loopSamples > 0) {
        decompilation.statistics.loopSample = loopSample;
        decompilation.statistics.loopFound = true;
    }
    
    /**
     * Добавляет команду в декомпилированный VGM вывод
     * 
     * Центральная функция для записи всех VGM команд в результирующий декомпилированный
     * код. Учитывает ограничения на количество команд (maxCommands) и опцию showAllCommands.
     * Также ведет статистику общего количества команд.
     * 
     * @param {Object} cmd - Объект команды с полями type, position, offset, raw, mnemonic, description, dataBytes
     */
    function addCommand(cmd) {
        if (maxCommands === 0 || showAllCommands || decompilation.commands.length < maxCommands) {
            // Специальная обработка для временных меток и меток петли
            if (cmd.type === 'TIME_MARK' || cmd.type === 'LOOP_MARK') {
                cmd.command = cmd.command; // Используем готовый форматированный текст
            } else {
                // Форматируем обычные команды согласно новому формату
                cmd.command = formatCommand(commandNumber, cmd.raw || [cmd.opcode || 0], cmd.mnemonic || cmd.description, cmd.dataBytes);
                commandNumber++; // Увеличиваем номер команды только для обычных команд
            }
            decompilation.commands.push(cmd);
        }
        
        // Статистика для всех команд, кроме временных меток
        if (cmd.type !== 'TIME_MARK' && cmd.type !== 'LOOP_MARK') {
            decompilation.statistics.totalCommands++;
        }
    }
    
    /**
     * Добавляет временные метки в декомпилированный код
     * 
     * \"VGM files use a sample rate of 44100 Hz. All wait times are given as a
     *  number of samples to wait at this rate.\"
     * 
     * Добавляет визуальные временные метки каждую секунду для улучшения читаемости
     * декомпилированного VGM кода. Помогает ориентироваться во времени воспроизведения.
     * 
     * @param {number} samples - Текущее количество samples с начала воспроизведения
     */
    function addTimeMark(samples) {
        const seconds = Math.floor(samples / SAMPLE_RATE);
        if (seconds > lastTimeMarkSeconds) {
            const timeStr = formatTime(seconds);
            addCommand({
                type: 'TIME_MARK',
                position: position,
                offset: `0x${position.toString(16).toUpperCase()}`,
                samples: samples,
                seconds: seconds,
                timeString: timeStr,
                command: `\n*** time ${timeStr} ***********`,
                description: `Временная метка`
            });
            decompilation.statistics.timeMarks.push(seconds);
            lastTimeMarkSeconds = seconds;
        }
    }
    
    /**
     * Проверяет и отмечает точку начала цикла (loop point)
     * 
     * \"4+4 bytes: Loop offset (32 bits, starting from 0x1C)
     *  Relative offset to loop point, or 0 if no looping. This is added to the VGM
     *  position 0x1C (where the VGM header ends) to get the absolute position from the
     *  beginning of the file.\"
     *
     * VGM файлы могут содержать информацию о цикле (loop). Эта функция следит за
     * текущей позицией в samples и добавляет визуальную метку в декомпилированный
     * код когда достигается точка начала цикла.
     * 
     * @param {number} samples - Текущее количество samples с начала воспроизведения
     */
    function checkLoopPoint(samples) {
        if (decompilation.statistics.loopFound && samples >= loopSample && !decompilation.loopMarkAdded) {
            const loopTimeStr = formatTime(Math.floor(loopSample / SAMPLE_RATE));
            const loopSampleStr = loopSample.toString();
            addCommand({
                type: 'LOOP_MARK',
                position: position,
                offset: `0x${position.toString(16).toUpperCase()}`,
                samples: loopSample,
                seconds: loopSample / SAMPLE_RATE,
                timeString: loopTimeStr,
                command: `\n*** LOOP POINT ${loopTimeStr} ***********`,
                description: `Точка начала цикла`
            });
            decompilation.loopMarkAdded = true;
        }
    }
    
    // Helper function для создания временной метки
    function createTimeMark(position, samples, seconds) {
        const timeStr = formatTime(seconds);
        return {
            type: 'TIME_MARK',
            position: position,
            offset: `0x${position.toString(16).toUpperCase()}`,
            samples: samples,
            seconds: seconds,
            timeString: timeStr,
            command: `\n***************** ${timeStr} **************`,
            description: `Временная метка`
        };
    }
    
    /**
     * Форматирует команду VGM в декомпилированный вид
     * 
     * Формат: 0000001:  62 00 10          WAIT (31)
     *         ^^^^^^^^  ^^^^^^^^^^^^^^^^^^^^^^^^^
     *         номер     hex байты + мнемоника
     * 
     * @param {number} commandNumber - Номер команды (начиная с 1)
     * @param {Array<number>} rawBytes - Байты команды
     * @param {string} mnemonic - Мнемоника команды
     * @param {Array<number>} dataBytes - Дополнительные данные (для массивов)
     * @returns {string} Отформатированная строка команды
     */
    function formatCommand(commandNumber, rawBytes, mnemonic, dataBytes = null) {
        const number = commandNumber.toString().padStart(7, '0');
        const hexBytes = rawBytes.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
        const hex = hexBytes.padEnd(24, ' '); // Выравнивание для hex части
        let result = `${number}:  ${hex}${mnemonic}`;
        
        // Если есть дополнительные данные, выводим их отдельными строками по 16 байт
        if (dataBytes && dataBytes.length > 0) {
            result += '\n'; // Пустая строка перед данными
            for (let i = 0; i < dataBytes.length; i += 16) {
                const chunk = dataBytes.slice(i, i + 16);
                const hexChunk = chunk.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ');
                const padding = ' '.repeat(16); // Отступ для выравнивания с основной командой
                result += `${padding}${hexChunk}`;
                if (i + 16 < dataBytes.length) {
                    result += '\n';
                }
            }
            result += '\n'; // Пустая строка после данных
        }
        
        return result;
    }
    
    /**
     * Форматирует время в формате MM:SS
     * 
     * Используется для отображения временных меток в декомпилированном VGM.
     * Преобразует количество секунд в стандартный формат времени.
     * 
     * @param {number} totalSeconds - Общее количество секунд
     * @returns {string} Время в формате "MM:SS"
     */
    function formatTime(totalSeconds) {
        const minutes = Math.floor(totalSeconds / 60);
        const seconds = totalSeconds % 60;
        return `${minutes.toString().padStart(2, '0')}:${seconds.toString().padStart(2, '0')}`;
    }
    
    while (position < vgmBuffer.length && (maxCommands === 0 || commandIndex < maxCommands * 2)) {
        const opcode = vgmBuffer[position];
        commandIndex++;
        
        // Проверяем точку лупа
        checkLoopPoint(currentSamples);
        
        // Добавляем временную метку каждую секунду
        if (showTimeMarks) {
            addTimeMark(currentSamples);
        }
        
        const cmd = {
            type: 'COMMAND',
            position: position,
            offset: `0x${position.toString(16).toUpperCase().padStart(6, '0')}`,
            opcode: `0x${opcode.toString(16).toUpperCase().padStart(2, '0')}`,
            samples: currentSamples,
            ticks: currentSamples,
            seconds: (currentSamples / SAMPLE_RATE).toFixed(3),
            raw: [],
            parameters: [],
            command: '',
            description: ''
        };
        
        // Парсинг команд по спецификации VGM v1.71
        if (opcode === 0x66) {
            // End of sound data
            cmd.raw = [opcode];
            cmd.mnemonic = 'END';
            cmd.description = 'End of sound data';
            addCommand(cmd);
            break;
            
        } else if (opcode === 0x67) {
            // \"0x67 0x66 tt ss ss ss ss (data)\"
            // Data block: contains compressed data or large data arrays
            if (position + 6 < vgmBuffer.length) {
                const blockMarker = vgmBuffer[position + 1]; // Should be 0x66
                const blockType = vgmBuffer[position + 2];
                const blockSize = vgmBuffer.readUInt32LE(position + 3);
                
                // Команда + маркер + тип + байты размера в основной строке
                const sizeBytes = Array.from(vgmBuffer.subarray(position + 3, position + 7));
                cmd.raw = [opcode, blockMarker, blockType, ...sizeBytes];
                
                // В dataBytes только сами данные, без заголовков
                if (blockSize > 0 && position + 7 + blockSize <= vgmBuffer.length) {
                    const dataArray = Array.from(vgmBuffer.subarray(position + 7, position + 7 + blockSize));
                    cmd.dataBytes = dataArray;
                    cmd.mnemonic = `DATA_BLOCK (type: 0x${blockType.toString(16).toUpperCase()}, size: ${blockSize})`;
                    cmd.description = `Data block: type=${blockType}, size=${blockSize} bytes`;
                    position += 7 + blockSize;
                } else {
                    cmd.dataBytes = null; // Нет данных, только заголовок
                    cmd.mnemonic = `DATA_BLOCK_HEADER (type: 0x${blockType.toString(16).toUpperCase()}, size: ${blockSize})`;
                    cmd.description = `Data block header only: type=${blockType}, size=${blockSize} bytes`;
                    position += 7;
                }
                
                decompilation.statistics.dataBlocks++;
                addCommand(cmd);
            } else {
                // Incomplete data block
                cmd.raw = [opcode];
                cmd.mnemonic = 'DATA_BLOCK_INCOMPLETE';
                cmd.description = 'Incomplete data block header';
                addCommand(cmd);
                position++;
            }
            
        } else if (opcode >= 0x70 && opcode <= 0x7F) {
            // \"0x7n : wait n+1 samples, n can range from 0 to 15.\"
            // Короткое ожидание: 0x70 = 1 sample, 0x7F = 16 samples
            const waitSamples = (opcode & 0x0F) + 1;
            cmd.raw = [opcode];
            cmd.mnemonic = `WAIT (${waitSamples})`;
            cmd.description = `Wait ${waitSamples} ticks (${(waitSamples / SAMPLE_RATE * 1000).toFixed(1)}ms)`;
            currentSamples += waitSamples;
            decompilation.statistics.waitCommands++;
            addCommand(cmd);
            position++;
            
        } else if (opcode === 0x61) {
            // Wait n samples
            if (position + 2 < vgmBuffer.length) {
                const waitSamples = vgmBuffer.readUInt16LE(position + 1);
                cmd.raw = [opcode, vgmBuffer[position + 1], vgmBuffer[position + 2]];
                cmd.mnemonic = `WAIT (${waitSamples})`;
                cmd.description = `Wait ${waitSamples} ticks (${(waitSamples / SAMPLE_RATE * 1000).toFixed(1)}ms)`;
                currentSamples += waitSamples;
                decompilation.statistics.waitCommands++;
                addCommand(cmd);
                position += 3;
            } else break;
            
        } else if (opcode === 0x62) {
            // "0x62       : wait 735 samples (60th of a second), a shortcut for
            //               0x61 0xdf 0x02"
            const waitSamples = 735;
            cmd.raw = [opcode];
            cmd.mnemonic = `WAIT60 (${waitSamples})`;
            cmd.description = `Wait ${waitSamples} ticks (16.7ms, 60Hz)`;
            currentSamples += waitSamples;
            decompilation.statistics.waitCommands++;
            addCommand(cmd);
            position++;
            
        } else if (opcode === 0x63) {
            // Wait 882 samples (50Hz) 
            const waitSamples = 882;
            cmd.raw = [opcode];
            cmd.mnemonic = `WAIT50 (${waitSamples})`;
            cmd.description = `Wait ${waitSamples} ticks (20.0ms, 50Hz)`;
            currentSamples += waitSamples;
            addCommand(cmd);
            position++;
                
        } else if (opcode >= 0x80 && opcode <= 0x8F) {
            // YM2612 DAC + wait
            const waitSamples = opcode & 0x0F;
            cmd.raw = [opcode];
            cmd.mnemonic = `YM2612DAC_WAIT (${waitSamples})`;
            cmd.description = `YM2612 DAC write then wait ${waitSamples} samples`;
            currentSamples += waitSamples;
            decompilation.statistics.writeCommands++;
            addCommand(cmd);
            position++;
            
        // Чип команды записи (2 параметра)
        } else if ([0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
                   0xA0, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF].includes(opcode)) {
            if (position + 2 < vgmBuffer.length) {
                const reg = vgmBuffer[position + 1];
                const data = vgmBuffer[position + 2];
                cmd.raw = [opcode, reg, data];
                cmd.parameters = [`0x${reg.toString(16).toUpperCase().padStart(2, '0')}`, `0x${data.toString(16).toUpperCase().padStart(2, '0')}`];
                
                // Определяем тип чипа по опкоду
                const chipCommands = {
                    0x50: 'PSG', 0x51: 'YM2413', 0x52: 'YM2612 Port 0', 0x53: 'YM2612 Port 1',
                    0x54: 'YM2151', 0x55: 'YM2203', 0x56: 'YM2608 Port 0', 0x57: 'YM2608 Port 1',
                    0x58: 'YM2610 Port 0', 0x59: 'YM2610 Port 1', 0x5A: 'YM3812', 0x5B: 'YM3526',
                    0x5C: 'Y8950', 0x5D: 'YMZ280B', 0x5E: 'YMF262 Port 0', 0x5F: 'YMF262 Port 1',
                    0xA0: 'AY8910', 0xB0: 'RF5C68', 0xB1: 'RF5C164', 0xB2: 'PWM', 0xB3: 'GameBoy DMG',
                    0xB4: 'NES APU', 0xB5: 'MultiPCM', 0xB6: 'uPD7759', 0xB7: 'OKIM6258', 0xB8: 'OKIM6295',
                    0xB9: 'HuC6280', 0xBA: 'K053260', 0xBB: 'Pokey', 0xBC: 'WonderSwan', 0xBD: 'SAA1099',
                    0xBE: 'ES5506', 0xBF: 'GA20'
                };
                
                const chipName = chipCommands[opcode] || 'Unknown';
                cmd.mnemonic = `${chipName} (0x${reg.toString(16).toUpperCase().padStart(2, '0')}, 0x${data.toString(16).toUpperCase().padStart(2, '0')})`;
                cmd.description = `${chipName} write: reg=0x${reg.toString(16).toUpperCase()}, data=0x${data.toString(16).toUpperCase()}`;
                decompilation.statistics.writeCommands++;
                addCommand(cmd);
                position += 3;
            } else break;
            
        } else {
            // Неизвестная или нереализованная команда
            cmd.raw = [opcode];
            cmd.mnemonic = `UNKNOWN (0x${opcode.toString(16).toUpperCase()})`;
            cmd.description = `Unknown/Unimplemented command`;
            addCommand(cmd);
            position++;
        }
    }
    
    // Финальные статистики
    decompilation.statistics.totalSamples = currentSamples;
    decompilation.statistics.totalSeconds = (currentSamples / SAMPLE_RATE).toFixed(3);
    
    return decompilation;
}

module.exports = {
    readVGZ,
    parseVGMHeader,
    parseVGMCommands,
    parseVGMHeader,
    analyzeVGM,
    analyzeVGZFile,
    analyzeGZIP,
    decompileVGM,
    CHIP_OFFSETS,
    VGM_COMMANDS
};

// ===================================================================
// CLI Interface
// ===================================================================

/**
 * Распаковка одного VGZ файла в VGM
 * @param {string} vgzPath - Путь к VGZ файлу
 * @param {string} outputPath - Путь для сохранения VGM файла (необязательно)
 */
function unpackVGZ(vgzPath, outputPath) {
    try {
        // Резолвим путь относительно текущего рабочего каталога
        const resolvedVgzPath = path.resolve(vgzPath);
        
        console.log(`Unpacking: ${vgzPath}`);
        
        const { data: vgmData, gzipInfo } = readVGZ(resolvedVgzPath);
        
        // Определяем имя выходного файла
        if (!outputPath) {
            outputPath = path.resolve(vgzPath.replace(/\.vgz$/i, '.vgm'));
        } else {
            outputPath = path.resolve(outputPath);
        }
        
        // Записываем VGM данные
        fs.writeFileSync(outputPath, vgmData);
        
        console.log(`✓ Unpacked: ${outputPath}`);
        console.log(`  Compressed: ${gzipInfo.compressedSize} bytes`);
        console.log(`  Uncompressed: ${gzipInfo.uncompressedSize} bytes`);
        console.log(`  Compression ratio: ${gzipInfo.compressionRatioText}`);
        
        return true;
    } catch (error) {
        console.error(`✗ Error unpacking ${vgzPath}: ${error.message}`);
        return false;
    }
}

/**
 * Распаковка всех VGZ файлов в текущем каталоге
 * @param {string} dirPath - Путь к каталогу (по умолчанию текущий)
 */
function unpackAllVGZ(dirPath = '.') {
    try {
        // Резолвим путь относительно текущего рабочего каталога
        const resolvedDirPath = path.resolve(dirPath);
        
        const files = fs.readdirSync(resolvedDirPath);
        const vgzFiles = files.filter(file => file.toLowerCase().endsWith('.vgz'));
        
        if (vgzFiles.length === 0) {
            console.log('No .vgz files found in current directory');
            return;
        }
        
        console.log(`Found ${vgzFiles.length} VGZ files to unpack:`);
        
        let successful = 0;
        let failed = 0;
        
        vgzFiles.forEach(vgzFile => {
            const vgzPath = path.join(resolvedDirPath, vgzFile);
            if (unpackVGZ(vgzPath)) {
                successful++;
            } else {
                failed++;
            }
        });
        
        console.log('\n' + '='.repeat(50));
        console.log(`Unpacking complete: ${successful} successful, ${failed} failed`);
        
    } catch (error) {
        console.error(`Error reading directory: ${error.message}`);
    }
}

/**
 * Отображение информации о заголовке VGM/VGZ файла
 * @param {string} filePath - Путь к файлу
 */
function showHeader(filePath) {
    try {
        // Резолвим путь относительно текущего рабочего каталога
        const resolvedPath = path.resolve(filePath);
        
        let vgmData;
        let compressionInfo = null;
        
        if (filePath.toLowerCase().endsWith('.vgz')) {
            const result = readVGZ(resolvedPath);
            vgmData = result.data;
            compressionInfo = result.gzipInfo;
        } else {
            vgmData = fs.readFileSync(resolvedPath);
        }
        
        const header = parseVGMHeader(vgmData);
        
        console.log('='.repeat(60));
        console.log(`VGM Header Information: ${path.basename(filePath)}`);
        console.log('='.repeat(60));
        
        if (compressionInfo) {
            console.log('Compression Info:');
            console.log(`  Format: VGZ (gzip compressed VGM)`);
            console.log(`  Compressed size: ${compressionInfo.compressedSize} bytes`);
            console.log(`  Uncompressed size: ${compressionInfo.uncompressedSize} bytes`);
            console.log(`  Compression ratio: ${compressionInfo.compressionRatioText}`);
            console.log(`  OS: ${compressionInfo.os}`);
            console.log('');
        }
        
        console.log('VGM Header:');
        console.log(`  Format: ${header.signature}`);
        console.log(`  Version: ${header.versionString} (${header.versionHex})`);
        console.log(`  File size: ${header.eofOffset} bytes`);
        console.log(`  Data offset: 0x${header.dataOffset.toString(16).toUpperCase()}`);
        console.log('');
        
        console.log('Timing:');
        console.log(`  Total samples: ${header.totalSamples}`);
        console.log(`  Total time: ${header.totalSeconds.toFixed(2)} seconds`);
        console.log(`  Sample rate: ${header.sampleRate} Hz`);
        
        if (header.hasLoop) {
            console.log(`  Loop offset: 0x${header.loopOffset.toString(16).toUpperCase()}`);
            console.log(`  Loop samples: ${header.loopSamples}`);
            console.log(`  Loop time: ${header.loopSeconds.toFixed(2)} seconds`);
            console.log(`  Intro time: ${header.introSeconds.toFixed(2)} seconds`);
        } else {
            console.log(`  Loop: No loop`);
        }
        console.log('');
        
        console.log('Sound Chips:');
        header.chips.forEach(chip => {
            const dualText = chip.isDualChip ? ' (Dual Chip)' : '';
            const clockText = typeof chip.clockMHz === 'number' ? `${chip.clockMHz.toFixed(3)} MHz` : '(clock info unavailable)';
            console.log(`  ${chip.name}: ${clockText}${dualText}`);
        });
        
        if (header.gd3Offset > 0) {
            console.log('');
            console.log('GD3 Tags:');
            try {
                const gd3 = parseGD3(vgmData, header.gd3Offset);
                console.log(`  Track: ${gd3.trackTitle || 'N/A'}`);
                console.log(`  Game: ${gd3.gameName || 'N/A'}`);
                console.log(`  System: ${gd3.systemName || 'N/A'}`);
                console.log(`  Author: ${gd3.trackAuthor || 'N/A'}`);
                console.log(`  Date: ${gd3.date || 'N/A'}`);
                console.log(`  Ripper: ${gd3.ripper || 'N/A'}`);
            } catch (err) {
                console.log(`  Error reading GD3: ${err.message}`);
            }
        }
        
    } catch (error) {
        console.error(`Error reading file ${filePath}: ${error.message}`);
    }
}

/**
 * Декомпиляция VGM/VGZ файла
 * @param {string} filePath - Путь к файлу
 * @param {Object} options - Опции декомпиляции
 */
function decompileFile(filePath, options = {}) {
    try {
        // Резолвим путь относительно текущего рабочего каталога
        const resolvedPath = path.resolve(filePath);
        
        let vgmData;
        
        if (filePath.toLowerCase().endsWith('.vgz')) {
            const result = readVGZ(resolvedPath);
            vgmData = result.data;
        } else {
            vgmData = fs.readFileSync(resolvedPath);
        }
        
        const decompileOptions = {
            maxCommands: options.maxCommands !== undefined ? options.maxCommands : 0, // 0 = без ограничений
            showTimeMarks: options.showTimeMarks !== false,
            showAllCommands: options.showAllCommands || false
        };
        
        const decompilation = decompileVGM(vgmData, decompileOptions);
        
        // Выводим заголовок
        console.log(`; VGM Decompilation: ${path.basename(filePath)}`);
        console.log(`; Generated by VGM-lib v1.71`);
        console.log(`; File: ${filePath}`);
        console.log(`; Size: ${vgmData.length} bytes`);
        console.log(`; Format: ${decompilation.header.signature} ${decompilation.header.versionString}`);
        console.log(`;`);
        
        // Выводим информацию об использованных чипах
        if (decompilation.header.chips.length > 0) {
            console.log(`; Sound chips:`);
            decompilation.header.chips.forEach(chip => {
                const dualText = chip.isDualChip ? ' (Dual)' : '';
                const clockText = typeof chip.clockMHz === 'number' ? `${chip.clockMHz.toFixed(3)} MHz` : '(clock info unavailable)';
                console.log(`; ${chip.name}: ${clockText}${dualText}`);
            });
            console.log(`;`);
        }
        
        // Выводим статистику
        console.log(`; Statistics:`);
        console.log(`; Total commands: ${decompilation.statistics.totalCommands}`);
        console.log(`; Total samples: ${decompilation.statistics.totalSamples}`);
        console.log(`; Total time: ${decompilation.statistics.totalSeconds} seconds`);
        
        if (decompilation.statistics.loopFound) {
            console.log(`; Loop sample: ${decompilation.statistics.loopSample}`);
        }
        console.log(`;`);
        console.log('');
        
        // Выводим команды
        decompilation.commands.forEach(cmd => {
            console.log(cmd.command);
        });
        
    } catch (error) {
        console.error(`Error decompiling file ${filePath}: ${error.message}`);
    }
}

/**
 * Показать справку по CLI командам
 */
function showHelp() {
    console.log('VGM Library CLI v1.71');
    console.log('Usage: vgm <command> [options]');
    console.log('');
    console.log('Commands:');
    console.log('  --header <file>         Show VGM/VGZ header information');
    console.log('  --decompile <file>      Decompile VGM/VGZ file to console');
    console.log('  --unpack <file>         Unpack single VGZ file to VGM');
    console.log('  --unpack-all [dir]      Unpack all VGZ files in directory');
    console.log('  --help                  Show this help');
    console.log('');
    console.log('Decompile options:');
    console.log('  --max-commands <n>      Limit decompiled commands (default: unlimited)');
    console.log('  --no-time-marks         Disable time markers in decompilation');
    console.log('  --show-all              Show all commands (ignore max-commands limit)');
    console.log('');
    console.log('Examples:');
    console.log('  vgm --header song.vgm');
    console.log('  vgm --decompile song.vgz > song_decompiled.txt');
    console.log('  vgm --unpack song.vgz');
    console.log('  vgm --unpack-all');
    console.log('  vgm --decompile song.vgm --max-commands 500 --no-time-marks');
}

// CLI Entry Point
if (require.main === module) {
    const args = process.argv.slice(2);
    
    if (args.length === 0 || args.includes('--help') || args.includes('-h')) {
        showHelp();
        process.exit(0);
    }
    
    const command = args[0];
    
    switch (command) {
        case '--header':
            if (args.length < 2) {
                console.error('Error: --header requires file path');
                console.error('Usage: vgm --header <file>');
                process.exit(1);
            }
            showHeader(args[1]);
            break;
            
        case '--decompile':
            if (args.length < 2) {
                console.error('Error: --decompile requires file path');
                console.error('Usage: vgm --decompile <file> [options]');
                process.exit(1);
            }
            
            const decompileOptions = {};
            
            // Парсим опции декомпиляции
            for (let i = 2; i < args.length; i++) {
                if (args[i] === '--max-commands' && i + 1 < args.length) {
                    decompileOptions.maxCommands = parseInt(args[i + 1]);
                    i++; // skip next arg
                } else if (args[i] === '--no-time-marks') {
                    decompileOptions.showTimeMarks = false;
                } else if (args[i] === '--show-all') {
                    decompileOptions.showAllCommands = true;
                }
            }
            
            decompileFile(args[1], decompileOptions);
            break;
            
        case '--unpack':
            if (args.length < 2) {
                console.error('Error: --unpack requires file path');
                console.error('Usage: vgm --unpack <file>');
                process.exit(1);
            }
            unpackVGZ(args[1]);
            break;
            
        case '--unpack-all':
            const targetDir = args.length >= 2 ? args[1] : '.';
            unpackAllVGZ(targetDir);
            break;
            
        default:
            console.error(`Unknown command: ${command}`);
            console.error('Use --help to see available commands');
            process.exit(1);
    }
}

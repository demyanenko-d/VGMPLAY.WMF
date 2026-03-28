// PluginRunner — loads a WMF plugin binary into emulated TSConfig memory,
// emulates WC kernel startup (P8000 dispatch), runs the Z80 CPU,
// and provides analysis/logging infrastructure.
//
// The test_data directory is used as disk root for file I/O emulation.
// VGM/VGZ files placed there can be loaded by the plugin as if they
// were on a real SD card.

using System;
using System.IO;
using System.Text;
using System.Collections.Generic;
using System.Linq;
using ZXMAK2.Engine.Cpu;
using ZXMAK2.Engine.Cpu.Processor;

namespace InflateDebugger
{
    public class PluginRunner
    {
        // ── TSConfig page assignments ─────────────────────────────────
        const byte PLGPG       = 0x61;   // plugin page base
        const byte TVBPG       = 0x20;   // megabuffer base
        const byte MTXPG       = 0x00;   // main text page
        const byte SYS_PAGE    = 0x05;   // WC system / FAT engine page
        const byte FONT_PAGE   = 0x01;   // font page
        const byte RESB_PAGE   = 0x1D;   // window save buffer page

        // Plugin page indices (relative; phys = PLGPG + idx)
        const byte PLG_CODE_PAGE  = 0;   // page 0: CODE+DATA (0x8000–0xBFFF)
        // Extra pages appended by build.bat:
        // page 1,2: (unused in current build)
        // page 3: freq_tables.bin
        // page 4: inflate.bin
        // page 5: cmdblocks.bin

        // ── Ports ─────────────────────────────────────────────────────
        const ushort PORT_BORDER = 0x00FE;     // ZX border
        const ushort PORT_TSCONF_BORDER = 0x0FAF;

        // Debug ports (same as inflate debugger)
        const ushort DBG_STATUS = 0xFFAF;
        const ushort DBG_SRC    = 0xFEAF;
        const ushort DBG_DST    = 0xFDAF;

        // OPL3 ports
        const ushort PORT_OPL3_ADDR0 = 0x0008;   // OPL3 address bank 0
        const ushort PORT_OPL3_DATA  = 0x0009;   // OPL3 data
        const ushort PORT_OPL3_ADDR1 = 0x000A;   // OPL3 address bank 1

        // AY ports
        const ushort PORT_AY_ADDR  = 0xFFFD;
        const ushort PORT_AY_DATA  = 0xBFFD;

        // INT position ports (TSConfig raster comparator)
        const ushort PORT_HSINT   = 0x22AF;    // HCNT[8:1], 0–223
        const ushort PORT_VSINTL  = 0x23AF;    // VCNT[7:0]
        const ushort PORT_VSINTH  = 0x24AF;    // VCNT[8] (bit 0 only)
        const ushort PORT_INTMASK = 0x2AAF;

        // ── Configuration ─────────────────────────────────────────────
        public long   MaxCycles { get; set; } = 2_000_000_000;  // 2B cycles
        public long   WatchdogLimit { get; set; } = 500_000_000; // 500M without WC call = hang
        public bool   VerboseWcCalls { get; set; } = true;
        public bool   VerbosePageSwitch { get; set; } = false;
        public bool   DumpOnExit { get; set; } = true;
        public string DumpDir { get; set; }
        public string DiskRootPath { get; set; }  // test_data directory

        // File to auto-open for the plugin (relative to DiskRootPath)
        public string TestFileName { get; set; }

        // ── Emulator state ────────────────────────────────────────────
        public TsMemory Mem { get; private set; }
        public CpuUnit  Cpu { get; private set; }
        public WcKernel Wc  { get; private set; }
        public CpuTrace Trace { get; private set; }

        private bool _finished;
        private long _lastWcCallTact;
        private int  _exitCode;

        // ISR emulation state
        private const ushort ISR_STUB_ADDR = 0x5BF4;  // IM2 stub handler (EI+RETI)
        private int _isrFireCount;

        // ── Raster-based INT position emulation ──────────────────────
        // TSConfig frame: 320 lines × 224 T-states/line = 71680 T at 3.5 MHz
        // Raster runs at 3.5 MHz regardless of CPU turbo mode.
        // We track raster in fixed-point: units = base_T × RASTER_SCALE.
        // This keeps all arithmetic integer across turbo modes.
        //
        // Turbo multipliers (raster_advance = cpu_delta × mult):
        //   3.5 MHz → mult=20  (20/20 = 1.0 raster T per CPU T)
        //   7   MHz → mult=10  (10/20 = 0.5)
        //  10   MHz → mult=7   ( 7/20 = 0.35, ≈14 MHz + wait states)
        //  28   MHz → mult=5   ( 5/20 = 0.25)

        private const int RASTER_SCALE = 20;
        private const int LINE_TACTS   = 224;           // T per line at 3.5 MHz
        private const int LINES_PER_FRAME = 320;
        private const long FRAME_RASTER = (long)LINE_TACTS * LINES_PER_FRAME * RASTER_SCALE; // 1 433 600

        private long _rasterPos;       // current raster position (fixed-point)
        private long _prevTact;        // previous Cpu.Tact for delta calc
        private int  _rasterMult = RASTER_SCALE;  // multiplier for current turbo

        // Programmed INT position (written by ISR via OUTI to ports)
        private int  _intLine;         // 9-bit VCNT: VSINTH[0]:VSINTL[7:0]
        private int  _intHPos;         // 8-bit HCNT: HSINT
        private long _intTargetRaster; // target in fixed-point units
        private bool _intFired;        // one-shot per target crossing

        // CPU frequency / turbo mode
        // SYSCONF[1:0]: 0=3.5, 1=7, 2=14, 3=28 MHz
        private int _turboMode;

        private static int GetRasterMult(int turbo) => turbo switch
        {
            0 => 20,  // 3.5 MHz → 1:1
            1 => 10,  // 7   MHz → 2:1
            2 =>  7,  // ~10 MHz (≈14 MHz + wait)
            3 =>  5,  // 28  MHz → 4:1
            _ => 20,
        };

        /// <summary>Recalculate INT target from current VSINT/HSINT values.</summary>
        private void RecalcIntTarget()
        {
            long baseTact = (long)_intLine * LINE_TACTS + _intHPos;
            _intTargetRaster = baseTact * RASTER_SCALE;
            _intFired = false;  // re-arm for new target
        }

        // OPL3 register tracking
        private byte _opl3Addr0, _opl3Addr1;
        private int _opl3WriteCount;

        // Debug port state
        private byte _lastDbgStatus;

        // ── Function breakpoints (addresses from vgmplay.noi) ─────────
        private static readonly Dictionary<ushort, string> _breakpoints = new()
        {
            { 0x8874, "load_vgm" },
            { 0xAEF5, "inflate_vgz" },
            { 0xAF68, "copy_pages_to_tap" },
            { 0x8DE6, "vgm_parse_header" },
            { 0x8AC1, "main" },
        };
        private readonly HashSet<ushort> _hitBreakpoints = new();
        private bool _postCptLogged;
        private bool _inflateResultLogged;
        private bool _parseHeaderResultLogged;

        // ── megabuf page 0 write tracker (first 16 bytes) ─────────────
        private struct MbWriteInfo { public byte Value; public long Cycle; public ushort Z80Addr; public ushort PC; }
        private MbWriteInfo[] _mbPg0First = new MbWriteInfo[16];
        private MbWriteInfo[] _mbPg0Last  = new MbWriteInfo[16];
        private MbWriteInfo[] _mbPg0Prev  = new MbWriteInfo[16]; // second-to-last write
        private int[]         _mbPg0Cnt   = new int[16];

        // ── statistics ────────────────────────────────────────────────
        public long TotalCycles => Cpu?.Tact ?? 0;
        public int  WcCallCount => Wc?.CallLog.Count ?? 0;
        public int  ViolationCount => Mem?.Violations.Count ?? 0;

        // ── Run ───────────────────────────────────────────────────────

        /// <summary>
        /// Load plugin binary and run it with WC emulation.
        /// Returns exit code (0 = normal exit, >0 = error/abort).
        /// </summary>
        public int Run(string wmfPath, string testFile = null)
        {
            Console.WriteLine("═══════════════════════════════════════════════════");
            Console.WriteLine("  WC Plugin Runner — Full Emulation");
            Console.WriteLine("═══════════════════════════════════════════════════");

            // ── Setup ──────────────────────────────────
            Mem = new TsMemory();
            Cpu = new CpuUnit();
            Wc = new WcKernel(Mem, Cpu);
            Trace = new CpuTrace(2000);

            // Wire memory write watcher for megabuf page 0, first 16 bytes
            bool _corruptionTrapped = false;
            Mem.OnWrite = (addr, page, offset, val) =>
            {
                if (page == TVBPG && offset < 16)
                {
                    var info = new MbWriteInfo { Value = val, Cycle = Cpu.Tact, Z80Addr = addr, PC = Cpu.regs.PC };
                    if (_mbPg0Cnt[offset] == 0)
                        _mbPg0First[offset] = info;
                    _mbPg0Prev[offset] = _mbPg0Last[offset];
                    _mbPg0Last[offset] = info;
                    _mbPg0Cnt[offset]++;

                    // Trap: write to page 0x20 via Win1 (addr 0x4000-0x7FFF) — the corruption!
                    if (addr >= 0x4000 && addr < 0x8000 && offset == 0 && !_corruptionTrapped)
                    {
                        _corruptionTrapped = true;
                        Console.ForegroundColor = ConsoleColor.Red;
                        Console.WriteLine($"\n  *** CORRUPTION TRAP at T={Cpu.Tact} ***");
                        Console.WriteLine($"  PC=0x{Cpu.regs.PC:X4} z80Addr=0x{addr:X4} val=0x{val:X2}");
                        Console.WriteLine($"  AF={Cpu.regs.AF:X4} BC={Cpu.regs.BC:X4} DE={Cpu.regs.DE:X4} HL={Cpu.regs.HL:X4}");
                        Console.WriteLine($"  SP={Cpu.regs.SP:X4} IX={Cpu.regs.IX:X4} IY={Cpu.regs.IY:X4}");
                        Console.WriteLine($"  Win=[{Mem.GetWindow(0):X2},{Mem.GetWindow(1):X2},{Mem.GetWindow(2):X2},{Mem.GetWindow(3):X2}]");
                        // Also show what's at the LDIR source (HL) if this is during LDIR at 0xC60E
                        Console.WriteLine($"  alt AF'={Cpu.regs._AF:X4} BC'={Cpu.regs._BC:X4} DE'={Cpu.regs._DE:X4} HL'={Cpu.regs._HL:X4}");
                        // Dump a few bytes of stack
                        Console.Write("  Stack: ");
                        for (int si = 0; si < 8; si++)
                            Console.Write($"{Mem.Read((ushort)(Cpu.regs.SP + si)):X2} ");
                        Console.WriteLine();
                        Console.ResetColor();
                    }
                }
            };

            if (!string.IsNullOrEmpty(DiskRootPath))
                Wc.DiskRootPath = DiskRootPath;

            if (VerboseWcCalls)
                Wc.Log = msg => Console.WriteLine(msg);

            if (!string.IsNullOrEmpty(testFile))
                TestFileName = testFile;

            // ── 1. Load WMF binary ─────────────────────
            if (!File.Exists(wmfPath))
            {
                Console.Error.WriteLine($"WMF file not found: {wmfPath}");
                return 1;
            }
            byte[] wmfData = File.ReadAllBytes(wmfPath);
            Console.WriteLine($"WMF file: {wmfPath}");
            Console.WriteLine($"WMF size: {wmfData.Length} bytes");

            LoadPlugin(wmfData);

            // ── 2. Set up WC environment ───────────────
            SetupWcEnvironment();

            // ── 3. Set up file for the plugin ──────────
            SetupTestFile();

            // ── 4. Set up CPU ──────────────────────────
            SetupCpu();

            // ── 5. Set up page protection ──────────────
            SetupProtection();

            // ── 6. Run ─────────────────────────────────
            Console.WriteLine();
            Console.WriteLine("─── Execution Start ─────────────────────────────");
            _exitCode = RunCpu();

            // ── 7. Analysis ────────────────────────────
            Console.WriteLine();
            Console.WriteLine("─── Post-Run Analysis ───────────────────────────");
            PrintAnalysis();

            if (DumpOnExit)
                DumpState("exit");

            return _exitCode;
        }

        // ═══════════════════════════════════════════════════════════════
        // Plugin loading
        // ═══════════════════════════════════════════════════════════════

        private void LoadPlugin(byte[] wmfData)
        {
            Console.WriteLine();
            Console.WriteLine("─── Loading Plugin ──────────────────────────────");

            // WMF format (doc/02_Plugin_Header_Structure.md):
            //   +0:   16 bytes reserved
            //   +16:  16 bytes "WildCommanderMDL" signature
            //   +32:  1 byte  version (0x10)
            //   +33:  1 byte  reserved
            //   +34:  1 byte  pages count
            //   +35:  1 byte  start page number (relative, loaded into Win2)
            //   +36:  12 bytes data blocks (6 × 2: page_index, size_in_512b_sectors)
            //   +512: data blocks follow

            const int HEADER_SIZE = 512;

            // Detect signature at offset 16
            bool hasSignature = wmfData.Length > 48 &&
                wmfData[16] == (byte)'W' && wmfData[17] == (byte)'i' &&
                wmfData[18] == (byte)'l' && wmfData[19] == (byte)'d';

            if (!hasSignature)
            {
                Console.WriteLine("  WARNING: No WildCommanderMDL signature found!");
                Console.WriteLine("  Loading as raw pages starting from PLGPG");
                int rawPages = (wmfData.Length + TsMemory.PageSize - 1) / TsMemory.PageSize;
                for (int p = 0; p < rawPages; p++)
                {
                    int srcOff = p * TsMemory.PageSize;
                    int len = Math.Min(TsMemory.PageSize, wmfData.Length - srcOff);
                    Array.Copy(wmfData, srcOff, Mem.GetPage(PLGPG + p), 0, len);
                }
            }
            else
            {
                byte version    = wmfData[32];
                byte pagesCount = wmfData[34];
                byte startPage  = wmfData[35];

                Console.WriteLine($"  Signature: WildCommanderMDL v0x{version:X2}");
                Console.WriteLine($"  Pages: {pagesCount}, start page: {startPage}");

                // Parse data blocks
                int dataOffset = HEADER_SIZE;
                for (int blk = 0; blk < 6; blk++)
                {
                    byte pageIdx = wmfData[36 + blk * 2];
                    byte sectors = wmfData[37 + blk * 2];
                    if (sectors == 0) continue;

                    int blkSize = sectors * 512;
                    int physPage = PLGPG + pageIdx;

                    Console.WriteLine($"  Block {blk}: page {pageIdx} (phys 0x{physPage:X2}), " +
                                      $"{sectors}×512 = {blkSize} bytes");

                    int len = Math.Min(blkSize, wmfData.Length - dataOffset);
                    if (len > 0)
                    {
                        byte[] dest = Mem.GetPage(physPage);
                        Array.Copy(wmfData, dataOffset, dest, 0, Math.Min(len, TsMemory.PageSize));

                        // If block > 16KB (multiple pages), handle overflow
                        if (len > TsMemory.PageSize)
                        {
                            Console.WriteLine($"    WARNING: Block exceeds 16KB, truncated");
                        }
                    }
                    dataOffset += blkSize;
                }

                // Show extensions
                Console.Write("  Extensions: ");
                for (int ext = 0; ext < 32; ext++)
                {
                    int off = 64 + ext * 3;
                    if (wmfData[off] == 0) break;
                    if (ext > 0) Console.Write(", ");
                    Console.Write(System.Text.Encoding.ASCII.GetString(wmfData, off, 3));
                }
                Console.WriteLine();

                // Show plugin name
                string name = System.Text.Encoding.ASCII.GetString(wmfData, 165, 32).TrimEnd('\0', ' ');
                Console.WriteLine($"  Name: \"{name}\"");
            }

            // Verify: show first bytes of code page at 0x8000
            Mem.SetWindow(2, PLGPG);  // Win2 = plugin page 0
            Console.Write("  Code @0x8000: ");
            for (int i = 0; i < 16; i++)
                Console.Write($"{Mem.Read((ushort)(0x8000 + i)):X2} ");
            Console.WriteLine();
        }

        // ═══════════════════════════════════════════════════════════════
        // WC environment setup
        // ═══════════════════════════════════════════════════════════════

        private void SetupWcEnvironment()
        {
            Console.WriteLine();
            Console.WriteLine("─── WC Environment ──────────────────────────────");

            // Window mapping (as WC P8000 sets up before CALL 0x8000):
            //   Win0 = TXT page (0x00)
            //   Win1 = WC system page (0x05)
            //   Win2 = Plugin code page (PLGPG = 0x61)
            //   Win3 = TXT page (0x00) — WC always sets this before calling plugin
            Mem.SetWindow(0, MTXPG);
            Mem.SetWindow(1, SYS_PAGE);
            Mem.SetWindow(2, PLGPG);
            Mem.SetWindow(3, MTXPG);

            // Initialize WC system variables
            Wc.SysVarPage = SYS_PAGE;
            Wc.InitSysVars();

            // Set up IM2 vector area at 0x5BFF (in Win1 = SYS_PAGE)
            // 0x5BFF = Win1 offset 0x1BFF
            // We'll put a dummy ISR address there for now

            // Set up the JP instruction at 0x6006 (in Win1 = SYS_PAGE)
            // 0x6006 = Win1 offset 0x2006
            // JP to itself (we trap this before execution)
            var sysPage = Mem.GetPage(SYS_PAGE);
            sysPage[0x2006] = 0xC3;  // JP
            sysPage[0x2007] = 0x06;  // lo
            sysPage[0x2008] = 0x60;  // hi → JP 0x6006 (infinite loop, but we intercept)

            // Fill font page with dummy data (printable chars)
            // Font is at FONT_PAGE (0x01), character bitmaps
            // We just leave it zeroed — text output is stubbed anyway

            Console.WriteLine($"  Windows: W0=0x{MTXPG:X2} W1=0x{SYS_PAGE:X2} W2=0x{PLGPG:X2} W3=0x{MTXPG:X2}");
            Console.WriteLine($"  SysVars @0x6000 initialized");
        }

        // ═══════════════════════════════════════════════════════════════
        // Test file setup
        // ═══════════════════════════════════════════════════════════════

        private void SetupTestFile()
        {
            if (string.IsNullOrEmpty(TestFileName)) return;

            Console.WriteLine();
            Console.WriteLine("─── Test File ───────────────────────────────────");

            string fullPath = TestFileName;
            if (!Path.IsPathRooted(fullPath) && !string.IsNullOrEmpty(DiskRootPath))
                fullPath = Path.Combine(DiskRootPath, TestFileName);

            if (!File.Exists(fullPath))
            {
                Console.Error.WriteLine($"  Test file not found: {fullPath}");
                return;
            }

            byte[] fileData = File.ReadAllBytes(fullPath);
            Wc.SetFileData(fileData);

            Console.WriteLine($"  File: {TestFileName}");
            Console.WriteLine($"  Size: {fileData.Length} bytes ({(fileData.Length + 16383) / 16384} pages)");

            // Detect VGZ
            if (fileData.Length > 2 && fileData[0] == 0x1F && fileData[1] == 0x8B)
                Console.WriteLine("  Format: VGZ (gzip-compressed VGM)");
            else if (fileData.Length > 4 && fileData[0] == 0x56 && fileData[1] == 0x67 &&
                     fileData[2] == 0x6D && fileData[3] == 0x20)
                Console.WriteLine("  Format: VGM (raw)");
        }

        // ═══════════════════════════════════════════════════════════════
        // CPU setup
        // ═══════════════════════════════════════════════════════════════

        private void SetupCpu()
        {
            Console.WriteLine();
            Console.WriteLine("─── CPU Setup ───────────────────────────────────");

            // Wire CPU bus
            Cpu.RDMEM    = addr => Mem.Read(addr);
            Cpu.RDMEM_M1 = addr => Mem.Read(addr);
            Cpu.WRMEM    = (addr, val) => Mem.Write(addr, val);
            Cpu.RDPORT   = HandlePortRead;
            Cpu.WRPORT   = HandlePortWrite;
            Cpu.RDNOMREQ = addr => { };
            Cpu.WRNOMREQ = addr => { };
            Cpu.RESET    = () => { };
            Cpu.NMIACK_M1 = () => { };
            // Clear INT on acknowledge — prevents re-trigger since we use pulse semantics
            Cpu.INTACK_M1 = () => { Cpu.INT = false; };
            Cpu.SCANSIG  = () => { };

            // Initial CPU state as WC P8000 sets before CALL 0x8000:
            //   A  = call type (0x00 = by file extension)
            //   BC = pointer to filename string (in Win2 area)
            //   HL = file size low word
            //   DE = file size high word
            //   IX = panel structure pointer (we'll use a dummy)
            //   SP = WC stack (somewhere in Win1 area, ~0x5BF0)

            // Prepare filename in plugin data area (Win2, 0x8000–0xBFFF)
            // We'll put it at end of DATA area, say 0xBF00
            string fname = TestFileName ?? "test.vgm";
            byte[] nameBytes = System.Text.Encoding.ASCII.GetBytes(fname);
            ushort nameAddr = 0xBF00;
            for (int i = 0; i < nameBytes.Length && i < 60; i++)
                Mem.WriteUnprotected((ushort)(nameAddr + i), nameBytes[i]);
            Mem.WriteUnprotected((ushort)(nameAddr + Math.Min(nameBytes.Length, 60)), 0);

            uint fileSize = Wc.FileSize;

            Cpu.regs.A = 0x00;  // call type = by extension
            Cpu.regs.BC = nameAddr;
            Cpu.regs.HL = (ushort)(fileSize & 0xFFFF);
            Cpu.regs.DE = (ushort)((fileSize >> 16) & 0xFFFF);
            Cpu.regs.IX = 0xBE00;  // dummy panel ptr
            Cpu.regs.SP = 0x5BF0;  // WC stack area
            Cpu.regs.PC = 0x8000;  // plugin entry point

            // Set A' = file extension index (0)
            // _AF is ushort: A' in high byte, F' in low byte
            Cpu.regs._AF = 0x0000;

            // Set DE' = number of files (say 10), HL' = current file number (1)
            Cpu.regs._DE = 10;
            Cpu.regs._HL = 1;

            // Push return address (sentinel 0x0000) so RET from main() goes to 0x0000
            Cpu.regs.SP -= 2;
            Mem.WriteUnprotected(Cpu.regs.SP, 0x00);
            Mem.WriteUnprotected((ushort)(Cpu.regs.SP + 1), 0x00);

            // Enable interrupts (plugin expects them for ISR)
            Cpu.IFF1 = false;  // Will be enabled by the plugin itself (EI after isr_init)
            Cpu.IFF2 = false;
            Cpu.IM = 2;        // IM 2 (plugin uses IM 2 for ISR)
            Cpu.INT = false;
            Cpu.NMI = false;
            Cpu.RST = false;
            Cpu.HALTED = false;

            // ── IM2 vector table setup ────────────────────────────────
            // WC uses I=0x5B. On INT, Z80 reads vector from (I*256+databus).
            // Data bus = 0xFF → vector at address 0x5BFF.
            // We place a stub ISR handler (EI + RETI) at 0x5BF4 and point
            // the vector there.  This prevents uninitialized-vector crashes
            // when copy_pages_to_tap or inflate_vgz does EI before the
            // plugin installs its real ISR handler.
            Cpu.regs.IR = (ushort)(0x5B << 8);  // I = 0x5B, R = 0
            {
                var sysP = Mem.GetPage(SYS_PAGE);
                // Stub handler at 0x5BF4 (offset 0x1BF4 in SYS_PAGE)
                sysP[0x1BF4] = 0xFB;  // EI
                sysP[0x1BF5] = 0xED;  // RETI prefix
                sysP[0x1BF6] = 0x4D;  // RETI
                // Vector at 0x5BFF → lo=0xF4, hi=0x5B → handler at 0x5BF4
                sysP[0x1BFF] = 0xF4;  // lo byte of handler address
                // 0x5C00 = offset 0x1C00, but that's page boundary
                // 0x5C00 is still in Win1 (0x4000–0x7FFF), offset 0x1C00 in SYS_PAGE
                sysP[0x1C00] = 0x5B;  // hi byte of handler address
            }
            Console.WriteLine("  IM2 stub handler at 0x5BF4 (EI+RETI), vector at 0x5BFF→0x5BF4");

            Cpu.Tact = 0;
            Cpu.FX = CpuModeIndex.None;
            Cpu.XFX = CpuModeEx.None;

            Console.WriteLine($"  PC=0x{Cpu.regs.PC:X4} SP=0x{Cpu.regs.SP:X4}");
            Console.WriteLine($"  A=0x{Cpu.regs.A:X2} BC=0x{Cpu.regs.BC:X4} DE=0x{Cpu.regs.DE:X4} HL=0x{Cpu.regs.HL:X4}");
            Console.WriteLine($"  File: \"{fname}\" ({fileSize} bytes) @ 0x{nameAddr:X4}");
        }

        // ═══════════════════════════════════════════════════════════════
        // Page protection
        // ═══════════════════════════════════════════════════════════════

        private void SetupProtection()
        {
            Console.WriteLine();
            Console.WriteLine("─── Page Protection ─────────────────────────────");

            // NOTE: WC system page (0x05) contains the stack at ~0x5BF0,
            // so we cannot write-protect it. The plugin legitimately writes there.
            // We only protect pages that should NEVER be written by the plugin.

            // Protect font page (read-only, plugin should never write fonts)
            Mem.ProtectPage(FONT_PAGE);
            Console.WriteLine($"  Protected: page 0x{FONT_PAGE:X2} (font)");

            // Hook for violation logging
            Mem.OnViolation = v =>
            {
                Console.ForegroundColor = ConsoleColor.Red;
                Console.WriteLine($"  !! {v} at tact={Cpu.Tact} PC=0x{Cpu.regs.PC:X4}");
                Console.ResetColor();
            };
        }

        // ═══════════════════════════════════════════════════════════════
        // Port I/O handlers
        // ═══════════════════════════════════════════════════════════════

        private byte HandlePortRead(ushort port)
        {
            // ZX keyboard ports (0xFE with specific A8-A15)
            // Return 0xFF = no key pressed (all bits high)
            if ((port & 0xFF) == 0xFE) return 0xFF;

            // AY register read
            if (port == PORT_AY_ADDR) return 0xFF;

            return 0xFF;
        }

        private void HandlePortWrite(ushort port, byte value)
        {
            // TSConfig page ports
            if (Mem.HandlePort(port, value))
            {
                if (VerbosePageSwitch)
                    Console.WriteLine($"  [{Cpu.Tact,10}] Page port 0x{port:X4} = 0x{value:X2}");
                return;
            }

            switch (port)
            {
                // ZX border
                case PORT_BORDER:
                    break;
                case PORT_TSCONF_BORDER:
                    break;

                // OPL3 ports
                case PORT_OPL3_ADDR0:
                    _opl3Addr0 = value;
                    break;
                case PORT_OPL3_DATA:
                    _opl3WriteCount++;
                    break;
                case PORT_OPL3_ADDR1:
                    _opl3Addr1 = value;
                    break;

                // AY ports
                case PORT_AY_ADDR:
                case PORT_AY_DATA:
                    break;

                // INT position ports (ISR uses OUTI to these)
                case PORT_VSINTH:   // 0x24AF — VCNT[8] (bit 0)
                    _intLine = (_intLine & 0xFF) | ((value & 0x01) << 8);
                    RecalcIntTarget();
                    break;
                case PORT_VSINTL:   // 0x23AF — VCNT[7:0]
                    _intLine = (_intLine & 0x100) | value;
                    RecalcIntTarget();
                    break;
                case PORT_HSINT:    // 0x22AF — HCNT[8:1], 0–223
                    _intHPos = value;
                    RecalcIntTarget();
                    break;
                case PORT_INTMASK:  // 0x2AAF
                    break;

                // Debug ports
                case DBG_STATUS:
                    _lastDbgStatus = value;
                    break;
                case DBG_SRC:
                case DBG_DST:
                    break;

                // TSConfig system ports
                case 0x20AF:  // SYSCONF (turbo bits [1:0])
                {
                    int newTurbo = value & 0x03;
                    if (newTurbo != _turboMode)
                    {
                        int oldMult = _rasterMult;
                        _turboMode = newTurbo;
                        _rasterMult = GetRasterMult(newTurbo);
                        Console.WriteLine($"  [TURBO] port 0x20AF = 0x{value:X2}  mode {_turboMode}  rasterMult {oldMult} → {_rasterMult}");
                    }
                    break;
                }
                case 0x2BAF:  // CACHECONF
                    break;

                default:
                    // Silently ignore unknown ports
                    break;
            }
        }

        // ═══════════════════════════════════════════════════════════════
        // Main execution loop
        // ═══════════════════════════════════════════════════════════════

        private int RunCpu()
        {
            _finished = false;
            _lastWcCallTact = 0;
            _rasterPos = 0;
            _prevTact  = 0;
            _intFired  = false;
            RecalcIntTarget();
            long progressTact = 0;
            bool inWin1Warned = false;  // only warn once about Win1 stray

            while (!_finished)
            {
                ushort pc = Cpu.regs.PC;

                // ── Sentinel: plugin returned to 0x0000 ──
                if (pc == 0x0000)
                {
                    byte exitA = Cpu.regs.A;
                    Console.ForegroundColor = ConsoleColor.Green;
                    Console.WriteLine($"\n  Plugin returned at cycle {Cpu.Tact}");
                    Console.WriteLine($"  A = 0x{exitA:X2} (exit code: {ExitCodeName(exitA)})");
                    Console.ResetColor();

                    // Dump stack area & VGM header 
                    Console.WriteLine($"  SP=0x{Cpu.regs.SP:X4}");
                    Console.Write("  Stack area [SP-8..SP+8]: ");
                    for (int si = -8; si < 8; si++)
                        Console.Write($"{Mem.Read((ushort)(Cpu.regs.SP + si)):X2} ");
                    Console.WriteLine();
                    // Dump megabuffer page 0 header (VGM signature area)
                    Console.Write("  Megabuf pg0 [0..15]: ");
                    var mbPage = Mem.GetPage(TVBPG);
                    for (int mi = 0; mi < 16; mi++)
                        Console.Write($"{mbPage[mi]:X2} ");
                    Console.WriteLine();
                    Console.Write("  Win3 page 0xC000 [0..15]: ");
                    byte w3now = Mem.GetWindow(3);
                    var w3pg = Mem.GetPage(w3now);
                    for (int mi = 0; mi < 16; mi++)
                        Console.Write($"{w3pg[mi]:X2} ");
                    Console.WriteLine($"  (Win3=0x{w3now:X2})");
                    // Show hit/missed breakpoints
                    Console.Write("  Breakpoints hit: ");
                    foreach (var bp in _breakpoints)
                        Console.Write($"{bp.Value}={(_hitBreakpoints.Contains(bp.Key) ? "YES" : "NO")} ");
                    Console.WriteLine();

                    _finished = true;
                    return 0;
                }

                // ── PC range check: detect stray into Win0/Win1 ──
                if (pc >= 0x4000 && pc < 0x8000 && pc != WcKernel.WC_ENTRY
                    && pc != ISR_STUB_ADDR && pc != ISR_STUB_ADDR + 1 && pc != ISR_STUB_ADDR + 2
                    && !inWin1Warned)
                {
                    // PC wandered into Win1 (system page) — NOT the WC_ENTRY
                    inWin1Warned = true;
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine($"\n  !! STRAY PC: 0x{pc:X4} (Win1 area) at cycle {Cpu.Tact}");
                    Console.ResetColor();
                    PrintCpuState();
                    Console.WriteLine("  Trace (last 20):");
                    Trace.Dump(20);
                    DumpState("stray_win1");
                    return 7;
                }
                if (pc < 0x4000 && pc != 0x0000 && !inWin1Warned)
                {
                    inWin1Warned = true;
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine($"\n  !! STRAY PC: 0x{pc:X4} (Win0 area) at cycle {Cpu.Tact}");
                    Console.ResetColor();
                    PrintCpuState();
                    Console.WriteLine("  Trace (last 20):");
                    Trace.Dump(20);
                    DumpState("stray_win0");
                    return 7;
                }

                // ── WC API trap ──
                if (Wc.TryIntercept())
                {
                    _lastWcCallTact = Cpu.Tact;

                    if (VerboseWcCalls && Wc.CallLog.Count > 0)
                    {
                        var last = Wc.CallLog[^1];
                        Console.WriteLine($"  [{Cpu.Tact,10}] WC 0x{last.FuncNum:X2} ({WcKernel.GetFuncName(last.FuncNum)})");

                        // After MNGCVPL: show what Win3 now maps to and first 4 bytes at 0xC000
                        if (last.FuncNum == WcKernel.FN_MNGCVPL)
                        {
                            byte w3 = Mem.GetWindow(3);
                            var pg = Mem.GetPage(w3);
                            Console.WriteLine($"    → Win3=0x{w3:X2}  @C000: {pg[0]:X2} {pg[1]:X2} {pg[2]:X2} {pg[3]:X2}");
                        }
                    }

                    continue;
                }

                // ── Function breakpoints ──
                if (_breakpoints.TryGetValue(pc, out string? bpName) && _hitBreakpoints.Add(pc))
                {
                    Console.ForegroundColor = ConsoleColor.Cyan;
                    Console.WriteLine($"\n  ► ENTER {bpName}() at cycle {Cpu.Tact}");
                    Console.WriteLine($"    A=0x{Cpu.regs.A:X2} HL=0x{Cpu.regs.HL:X4} DE=0x{Cpu.regs.DE:X4} BC=0x{Cpu.regs.BC:X4} SP=0x{Cpu.regs.SP:X4}");
                    Console.WriteLine($"    Win: [{Mem.GetWindow(0):X2} {Mem.GetWindow(1):X2} {Mem.GetWindow(2):X2} {Mem.GetWindow(3):X2}]");
                    // For copy_pages_to_tap / inflate_vgz: show stack (return address)
                    Console.Write("    Stack: ");
                    for (int si = 0; si < 12; si++)
                        Console.Write($"{Mem.Read((ushort)(Cpu.regs.SP + si)):X2} ");
                    Console.WriteLine();
                    Console.ResetColor();
                }

                // ── Post copy_pages_to_tap trace (0x8903-0x8920) ──
                if (pc >= 0x8903 && pc <= 0x8920 && !_postCptLogged)
                {
                    Console.ForegroundColor = ConsoleColor.DarkYellow;
                    Console.Write($"  [T{Cpu.Tact}] PC={pc:X4} op={Mem.Read(pc):X2}");
                    Console.Write($" AF={Cpu.regs.AF:X4} BC={Cpu.regs.BC:X4} DE={Cpu.regs.DE:X4} HL={Cpu.regs.HL:X4} SP={Cpu.regs.SP:X4}");
                    Console.WriteLine($" W=[{Mem.GetWindow(0):X2},{Mem.GetWindow(1):X2},{Mem.GetWindow(2):X2},{Mem.GetWindow(3):X2}]");
                    Console.ResetColor();
                    if (pc == 0x8920) _postCptLogged = true;  // stop after range
                }

                // ── Post inflate_vgz result (0x8917 = or a,a after call _inflate_vgz) ──
                if (pc == 0x8917 && !_inflateResultLogged)
                {
                    _inflateResultLogged = true;
                    Console.ForegroundColor = ConsoleColor.Yellow;
                    Console.WriteLine($"\n  ◄ inflate_vgz() returned A=0x{Cpu.regs.A:X2} (0=OK) at cycle {Cpu.Tact}");
                    // Dump megabuf page 0 first 128 bytes (decompressed VGM header)
                    byte[] mbPg0 = Mem.GetPage(TVBPG);
                    Console.WriteLine("  Decompressed VGM header (megabuf pg0, first 128 bytes):");
                    for (int row = 0; row < 8; row++)
                    {
                        Console.Write($"    {row * 16:X4}: ");
                        for (int col = 0; col < 16; col++)
                            Console.Write($"{mbPg0[row * 16 + col]:X2} ");
                        Console.WriteLine();
                    }
                    // Write tracker for first 16 bytes
                    Console.ForegroundColor = ConsoleColor.Cyan;
                    Console.WriteLine("  Megabuf pg0 write tracker (first 16 bytes):");
                    for (int i = 0; i < 16; i++)
                    {
                        if (_mbPg0Cnt[i] == 0)
                            Console.WriteLine($"    [{i:X2}] NEVER WRITTEN  (current=0x{mbPg0[i]:X2})");
                        else
                        {
                            Console.Write($"    [{i:X2}] writes={_mbPg0Cnt[i]}");
                            Console.Write($" first=(0x{_mbPg0First[i].Value:X2} @T{_mbPg0First[i].Cycle} z80=0x{_mbPg0First[i].Z80Addr:X4} PC=0x{_mbPg0First[i].PC:X4})");
                            if (_mbPg0Cnt[i] >= 2)
                                Console.Write($" prev=(0x{_mbPg0Prev[i].Value:X2} @T{_mbPg0Prev[i].Cycle} z80=0x{_mbPg0Prev[i].Z80Addr:X4} PC=0x{_mbPg0Prev[i].PC:X4})");
                            Console.Write($" last=(0x{_mbPg0Last[i].Value:X2} @T{_mbPg0Last[i].Cycle} z80=0x{_mbPg0Last[i].Z80Addr:X4} PC=0x{_mbPg0Last[i].PC:X4})");
                            Console.WriteLine();
                        }
                    }
                    Console.ResetColor();
                }

                // ── Post vgm_parse_header result (0x8BBA = or a,a after call _vgm_parse_header) ──
                if (pc == 0x8BBA && !_parseHeaderResultLogged)
                {
                    _parseHeaderResultLogged = true;
                    Console.ForegroundColor = ConsoleColor.Yellow;
                    Console.WriteLine($"\n  ◄ vgm_parse_header() returned A=0x{Cpu.regs.A:X2} (0=VGM_OK) at cycle {Cpu.Tact}");
                    // If error, dump the Win3 view of header (should be megabuf pg0 mapped by mngcvpl(0))
                    if (Cpu.regs.A != 0)
                    {
                        Console.ForegroundColor = ConsoleColor.Red;
                        Console.WriteLine("  !! VGM PARSE HEADER FAILED — dumping Win3 (megabuf pg0):");
                        byte w3 = Mem.GetWindow(3);
                        byte[] pg = Mem.GetPage(w3);
                        for (int row = 0; row < 16; row++)
                        {
                            Console.Write($"    {row * 16:X4}: ");
                            for (int col = 0; col < 16; col++)
                                Console.Write($"{pg[row * 16 + col]:X2} ");
                            Console.WriteLine();
                        }
                    }
                    Console.ResetColor();
                }

                // ── Record trace entry ──
                {
                    var entry = new TraceEntry
                    {
                        Tact = Cpu.Tact,
                        PC = pc,
                        Opcode = Mem.Read(pc),
                        AF = Cpu.regs.AF, BC = Cpu.regs.BC,
                        DE = Cpu.regs.DE, HL = Cpu.regs.HL,
                        AF2 = Cpu.regs._AF, BC2 = Cpu.regs._BC,
                        DE2 = Cpu.regs._DE, HL2 = Cpu.regs._HL,
                        IX = Cpu.regs.IX, IY = Cpu.regs.IY,
                        SP = Cpu.regs.SP,
                        Win0 = Mem.GetWindow(0), Win1 = Mem.GetWindow(1),
                        Win2 = Mem.GetWindow(2), Win3 = Mem.GetWindow(3),
                        WriteAddr = 0xFFFF,
                        WcFuncNum = 0xFF
                    };
                    Trace.Record(ref entry);
                }

                // ── Execute one instruction ──
                Cpu.ExecCycle();

                // ── Raster-based INT emulation ──
                // Advance raster by CPU T-states consumed, scaled to 3.5 MHz base.
                // Fire INT when raster crosses programmed VSINT/HSINT position.
                {
                    long cpuDelta = Cpu.Tact - _prevTact;
                    _prevTact = Cpu.Tact;

                    long prevRaster = _rasterPos;
                    _rasterPos += cpuDelta * _rasterMult;

                    bool crossed = false;
                    if (_rasterPos >= FRAME_RASTER)
                    {
                        // Old-frame tail: check with previous _intFired state
                        if (!_intFired && _intTargetRaster > prevRaster)
                            crossed = true;

                        _rasterPos %= FRAME_RASTER;
                        _intFired = false;   // new frame → re-arm

                        // New-frame start: target near raster origin
                        if (!crossed && _intTargetRaster <= _rasterPos)
                            crossed = true;
                    }
                    else
                    {
                        crossed = !_intFired &&
                            _intTargetRaster > prevRaster && _intTargetRaster <= _rasterPos;
                    }

                    if (crossed)
                    {
                        _intFired = true;
                        Cpu.INT = true;
                        _isrFireCount++;
                        if (_isrFireCount <= 5 || _isrFireCount % 100000 == 0)
                        {
                            int line = (int)(_intTargetRaster / RASTER_SCALE / LINE_TACTS);
                            int hpos = (int)(_intTargetRaster / RASTER_SCALE % LINE_TACTS);
                            Console.WriteLine($"  [ISR #{_isrFireCount}] T={Cpu.Tact} line={line} hpos={hpos} PC=0x{Cpu.regs.PC:X4}");
                        }
                    }
                }
                if (_lastWcCallTact > 0 && (Cpu.Tact - _lastWcCallTact) > WatchdogLimit)
                {
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine($"\n  WATCHDOG: {Cpu.Tact - _lastWcCallTact} cycles since last WC call");
                    Console.WriteLine($"  PC=0x{pc:X4} SP=0x{Cpu.regs.SP:X4}");
                    Console.ResetColor();
                    PrintCpuState();
                    return 4;
                }

                // ── Max cycles ──
                if (Cpu.Tact > MaxCycles)
                {
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine($"\n  MAX CYCLES ({MaxCycles}) exceeded");
                    Console.ResetColor();
                    return 5;
                }

                // ── HALT detection ──
                if (Cpu.HALTED)
                {
                    // In IM2, HALT waits for next INT — that's normal for ISR idle
                    // But if interrupts are disabled + HALT, it's a deadlock
                    if (!Cpu.IFF1)
                    {
                        Console.ForegroundColor = ConsoleColor.Red;
                        Console.WriteLine($"\n  DEADLOCK: HALT with DI at PC=0x{pc:X4}");
                        Console.ResetColor();
                        return 6;
                    }
                }

                // ── Progress indicator every 100M cycles ──
                if (Cpu.Tact - progressTact > 100_000_000)
                {
                    progressTact = Cpu.Tact;
                    Console.Write($"\r  Cycles: {Cpu.Tact / 1_000_000}M  WC calls: {Wc.CallLog.Count}  OPL3 writes: {_opl3WriteCount}          ");
                }
            }

            return 0;
        }

        // ═══════════════════════════════════════════════════════════════
        // Analysis and dumps
        // ═══════════════════════════════════════════════════════════════

        private void PrintCpuState()
        {
            var r = Cpu.regs;
            Console.WriteLine($"    AF={r.AF:X4} BC={r.BC:X4} DE={r.DE:X4} HL={r.HL:X4}");
            Console.WriteLine($"    AF'={r._AF:X4} BC'={r._BC:X4} DE'={r._DE:X4} HL'={r._HL:X4}");
            Console.WriteLine($"    IX={r.IX:X4} IY={r.IY:X4} SP={r.SP:X4} PC={r.PC:X4}");
            Console.WriteLine($"    IFF1={Cpu.IFF1} IFF2={Cpu.IFF2} IM={Cpu.IM} HALTED={Cpu.HALTED}");
            Console.WriteLine($"    Win0=0x{Mem.GetWindow(0):X2} Win1=0x{Mem.GetWindow(1):X2} Win2=0x{Mem.GetWindow(2):X2} Win3=0x{Mem.GetWindow(3):X2}");

            Console.Write("    Code @PC: ");
            for (int i = 0; i < 8; i++)
                Console.Write($"{Mem.Read((ushort)(r.PC + i)):X2} ");
            Console.WriteLine();

            Console.Write("    Stack: ");
            for (int i = 0; i < 8; i++)
                Console.Write($"{Mem.Read((ushort)(r.SP + i)):X2} ");
            Console.WriteLine();
        }

        private void PrintAnalysis()
        {
            Console.WriteLine($"  Total cycles:      {Cpu.Tact:N0}");
            Console.WriteLine($"  WC API calls:      {Wc.CallLog.Count}");
            Console.WriteLine($"  OPL3 writes:       {_opl3WriteCount}");
            Console.WriteLine($"  Violations:        {Mem.Violations.Count}");
            Console.WriteLine($"  Trace entries:     {Trace.Count}");
            Console.WriteLine($"  Dirty pages:       {Mem.DirtyPages.Count}");

            // WC function frequency
            if (Wc.CallLog.Count > 0)
            {
                Console.WriteLine();
                Console.WriteLine("  WC function frequency:");
                var freq = Wc.CallLog
                    .GroupBy(c => c.FuncNum)
                    .OrderByDescending(g => g.Count())
                    .Take(15);
                foreach (var g in freq)
                    Console.WriteLine($"    0x{g.Key:X2} ({WcKernel.GetFuncName(g.Key),-12}): {g.Count()}");
            }

            // Protection violations summary
            if (Mem.Violations.Count > 0)
            {
                Console.ForegroundColor = ConsoleColor.Red;
                Console.WriteLine();
                Console.WriteLine($"  !! {Mem.Violations.Count} PROTECTION VIOLATIONS:");
                foreach (var v in Mem.Violations.Take(10))
                    Console.WriteLine($"    {v}");
                if (Mem.Violations.Count > 10)
                    Console.WriteLine($"    ... and {Mem.Violations.Count - 10} more");
                Console.ResetColor();
            }
        }

        private void DumpState(string reason)
        {
            if (string.IsNullOrEmpty(DumpDir)) return;

            try
            {
                Directory.CreateDirectory(DumpDir);

                // CPU trace
                Trace.DumpToFile(Path.Combine(DumpDir, $"{reason}_trace.txt"));
                Console.WriteLine($"  Dumped: {reason}_trace.txt");

                // WC call log
                File.WriteAllText(
                    Path.Combine(DumpDir, $"{reason}_wc_calls.txt"),
                    Wc.DumpCallLog(200));
                Console.WriteLine($"  Dumped: {reason}_wc_calls.txt");

                // CPU state
                using (var sw = new StreamWriter(Path.Combine(DumpDir, $"{reason}_cpu_state.txt")))
                {
                    var r = Cpu.regs;
                    sw.WriteLine($"Reason: {reason}");
                    sw.WriteLine($"Tact: {Cpu.Tact}");
                    sw.WriteLine($"AF={r.AF:X4} BC={r.BC:X4} DE={r.DE:X4} HL={r.HL:X4}");
                    sw.WriteLine($"AF'={r._AF:X4} BC'={r._BC:X4} DE'={r._DE:X4} HL'={r._HL:X4}");
                    sw.WriteLine($"IX={r.IX:X4} IY={r.IY:X4} SP={r.SP:X4} PC={r.PC:X4}");
                    sw.WriteLine($"Wins: {Mem.GetWindow(0):X2} {Mem.GetWindow(1):X2} {Mem.GetWindow(2):X2} {Mem.GetWindow(3):X2}");
                    sw.WriteLine($"IFF1={Cpu.IFF1} IM={Cpu.IM}");
                }
                Console.WriteLine($"  Dumped: {reason}_cpu_state.txt");

                // Violations
                if (Mem.Violations.Count > 0)
                {
                    using var vw = new StreamWriter(Path.Combine(DumpDir, $"{reason}_violations.txt"));
                    foreach (var v in Mem.Violations)
                        vw.WriteLine(v);
                    Console.WriteLine($"  Dumped: {reason}_violations.txt ({Mem.Violations.Count} violations)");
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"  Dump error: {ex.Message}");
            }
        }

        private static string ExitCodeName(byte code) => code switch
        {
            0 => "ESC/Exit",
            1 => "Unrecognized (pass to next plugin)",
            2 => "Next file",
            3 => "Reload directory",
            4 => "Previous file",
            _ => $"unknown ({code})"
        };
    }
}

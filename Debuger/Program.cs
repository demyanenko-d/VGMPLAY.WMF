// InflateDebugger — Z80 emulator for debugging TSConfig inflate decompressor
//
// Loads VGZ file into emulated TSConfig memory pages (as tape loader would),
// loads inflate.bin into Win3, sets up registers as inflate_call.s would,
// and runs the Z80 CPU with debug port monitoring and watchdog.
//
// Usage: dotnet run [--vgz <path>] [--ref <path>] [--inflate <path>]
//                   [--dump-on-hang] [--max-cycles <N>] [--watchdog <N>]

using System;
using System.IO;
using System.IO.Compression;
using System.Collections.Generic;
using System.Linq;
using ZXMAK2.Engine.Cpu;
using ZXMAK2.Engine.Cpu.Processor;

namespace InflateDebugger
{
    class Program
    {
        // ── TSConfig page assignments (matching real WC plugin) ──
        const byte SRC_START_PAGE  = 0xA1;   // TAP pages for VGZ source
        const byte DST_START_PAGE  = 0x20;   // megabuffer for decompressed output
        const byte INFLATE_PAGE    = 0x6C;   // inflate code page (base 0x67 + 5)
        const byte PAGE_DECODE     = 0xEA;   // decode working buffer page
        const byte WC_BASE_PAGE    = 0x67;   // WC plugin base page

        // ── Ports ──
        const ushort DBG_STATUS = 0xFFAF;
        const ushort DBG_SRC    = 0xFEAF;
        const ushort DBG_DST    = 0xFDAF;

        // ── Defaults ──
        const long   DEFAULT_MAX_CYCLES = 500_000_000;  // 500M cycles max
        const long   DEFAULT_WATCHDOG   = 200_000;      // 200K cycles without dbg port = hang

        // ── State ──
        static TsMemory mem = new();
        static CpuUnit cpu = new();

        static long lastDbgCycle = 0;
        static long watchdogLimit = DEFAULT_WATCHDOG;
        static long maxCycles = DEFAULT_MAX_CYCLES;
        static bool dumpOnHang = true;
        static bool finished = false;
        static byte lastDbgStatus = 0;
        static byte lastDbgSrc = 0;
        static byte lastDbgDst = 0;
        static int srcPagesLoaded = 0;
        static string? refPath = null;
        static string dumpDir = "";

        // Track which pages were written as destination
        static HashSet<int> dstPagesWritten = new();

        static int Main(string[] args)
        {
            string basePath = FindProjectRoot();
            string vgzPath = Path.Combine(basePath,
                @"DiskRef\Music\OPL2\Arcade\_\Act-Fancer_Cybernetick_Hyper_Weapon_(Arcade)\02 Sci-Vax (BGM 1).vgz");
            string inflatePath = Path.Combine(basePath, @"src_sdcc\build\inflate.bin");
            refPath = Path.Combine(basePath, @"Debuger\ref.vgm");
            dumpDir = Path.Combine(basePath, @"Debuger\dumps");

            // Parse args
            for (int i = 0; i < args.Length; i++)
            {
                switch (args[i])
                {
                    case "--vgz" when i + 1 < args.Length:      vgzPath = args[++i]; break;
                    case "--ref" when i + 1 < args.Length:       refPath = args[++i]; break;
                    case "--inflate" when i + 1 < args.Length:   inflatePath = args[++i]; break;
                    case "--max-cycles" when i + 1 < args.Length: maxCycles = long.Parse(args[++i]); break;
                    case "--watchdog" when i + 1 < args.Length:  watchdogLimit = long.Parse(args[++i]); break;
                    case "--dump-on-hang":                       dumpOnHang = true; break;
                    case "--no-dump":                            dumpOnHang = false; break;
                }
            }

            Console.WriteLine("═══════════════════════════════════════════════════");
            Console.WriteLine("  TSConfig Inflate Debugger");
            Console.WriteLine("═══════════════════════════════════════════════════");

            // ─── 1. Load VGZ file ───
            if (!File.Exists(vgzPath))
            {
                Console.Error.WriteLine($"VGZ file not found: {vgzPath}");
                return 1;
            }
            byte[] vgzData = File.ReadAllBytes(vgzPath);
            Console.WriteLine($"VGZ file: {vgzPath}");
            Console.WriteLine($"VGZ size: {vgzData.Length} bytes");

            // ─── 2. Load inflate.bin ───
            if (!File.Exists(inflatePath))
            {
                Console.Error.WriteLine($"inflate.bin not found: {inflatePath}");
                return 1;
            }
            byte[] inflateData = File.ReadAllBytes(inflatePath);
            Console.WriteLine($"inflate.bin: {inflateData.Length} bytes");

            // ─── 3. Set up TSConfig memory ───
            SetupMemory(vgzData, inflateData);

            // ─── 4. Set up CPU ───
            SetupCpu();

            // ─── 5. Run ───
            Console.WriteLine();
            Console.WriteLine("─── Execution Start ─────────────────────────────");
            Console.WriteLine();

            int exitCode = RunCpu();

            // ─── 6. Post-run analysis ───
            Console.WriteLine();
            Console.WriteLine("─── Post-Run Analysis ───────────────────────────");
            AnalyzeResults();

            return exitCode;
        }

        static string FindProjectRoot()
        {
            // Walk up from current dir or exe dir looking for Debuger folder
            string? dir = AppDomain.CurrentDomain.BaseDirectory;
            for (int i = 0; i < 10 && dir != null; i++)
            {
                if (Directory.Exists(Path.Combine(dir, "Debuger")) &&
                    Directory.Exists(Path.Combine(dir, "src_sdcc")))
                    return dir;
                dir = Path.GetDirectoryName(dir);
            }
            // Try current directory
            dir = Environment.CurrentDirectory;
            for (int i = 0; i < 10 && dir != null; i++)
            {
                if (Directory.Exists(Path.Combine(dir, "Debuger")) &&
                    Directory.Exists(Path.Combine(dir, "src_sdcc")))
                    return dir;
                dir = Path.GetDirectoryName(dir);
            }
            return @"D:\zx_projects\OplPlug";
        }

        static void SetupMemory(byte[] vgzData, byte[] inflateData)
        {
            Console.WriteLine();
            Console.WriteLine("─── Memory Setup ────────────────────────────────");

            // Load VGZ into TAP pages (0xA1+) — as WC tape loader would
            srcPagesLoaded = mem.LoadToPages(vgzData, SRC_START_PAGE);
            Console.WriteLine($"VGZ loaded to pages 0x{SRC_START_PAGE:X2}–0x{SRC_START_PAGE + srcPagesLoaded - 1:X2} ({srcPagesLoaded} pages)");

            // Load inflate.bin into inflate page (0x6C)
            // inflate.bin is padded to fill code + RAM tables within one 16KB page
            if (inflateData.Length > TsMemory.PageSize)
            {
                Console.WriteLine($"WARNING: inflate.bin ({inflateData.Length}) > 16KB, truncating");
            }
            Array.Copy(inflateData, 0, mem.GetPage(INFLATE_PAGE), 0,
                Math.Min(inflateData.Length, TsMemory.PageSize));
            Console.WriteLine($"inflate.bin loaded to page 0x{INFLATE_PAGE:X2}");

            // Set up initial window mapping:
            // Win0 = source page 0xA1 (set by inflate entry code)
            // Win1 = don't care yet (inflate sets it)
            // Win2 = WC base page 0x67 (current plugin page — inflate will remap to PAGE_DECODE)
            // Win3 = inflate page 0x6C (set by inflate_call.s before JP 0xC000)
            mem.SetWindow(0, SRC_START_PAGE);   // Will be set by inflate entry
            mem.SetWindow(1, 0x00);             // Don't care — inflate sets Win1
            mem.SetWindow(2, WC_BASE_PAGE);     // Current Win2 = plugin base
            mem.SetWindow(3, INFLATE_PAGE);     // inflate code

            // Set up WC system variables at 0x6000–0x6003
            // These are in the page mapped to Win1 at offset 0x2000
            // Actually in the real system, Win1 maps some WC page.
            // We need 0x6000–0x6003 readable. Let's put a known page in Win1
            // that has these values. In WC, Win1 = page at sys var area.
            // For simplicity: map Win1 to a scratch page, write sys vars there
            byte sysVarPage = 0xFE;  // use a spare page for sys vars
            mem.SetWindow(1, sysVarPage);
            // 0x6000 = offset 0x2000 in Win1
            // Write the page values as WC would have them:
            mem.GetPage(sysVarPage)[0x2000] = SRC_START_PAGE; // 0x6000 = Win0 page
            mem.GetPage(sysVarPage)[0x2001] = sysVarPage;     // 0x6001 = Win1 page
            mem.GetPage(sysVarPage)[0x2002] = WC_BASE_PAGE;   // 0x6002 = Win2 page (plugin base)
            mem.GetPage(sysVarPage)[0x2003] = INFLATE_PAGE;   // 0x6003 = Win3 page

            Console.WriteLine($"Windows: W0=0x{SRC_START_PAGE:X2} W1=0x{sysVarPage:X2} W2=0x{WC_BASE_PAGE:X2} W3=0x{INFLATE_PAGE:X2}");
            Console.WriteLine($"Sys vars @0x6000: [{SRC_START_PAGE:X2} {sysVarPage:X2} {WC_BASE_PAGE:X2} {INFLATE_PAGE:X2}]");

            // Display first bytes of inflate at 0xC000
            Console.Write("inflate @0xC000: ");
            for (int i = 0; i < 16; i++)
                Console.Write($"{mem.Read((ushort)(0xC000 + i)):X2} ");
            Console.WriteLine();
        }

        static void SetupCpu()
        {
            Console.WriteLine();
            Console.WriteLine("─── CPU Setup ───────────────────────────────────");

            // Wire CPU bus to TSConfig memory
            cpu.RDMEM = (addr) => mem.Read(addr);
            cpu.RDMEM_M1 = (addr) => mem.Read(addr);
            cpu.WRMEM = (addr, val) => mem.Write(addr, val);
            cpu.RDPORT = (port) => 0xFF;  // unused reads return 0xFF
            cpu.WRPORT = HandlePortWrite;
            cpu.RDNOMREQ = (addr) => { };
            cpu.WRNOMREQ = (addr) => { };
            cpu.RESET = () => { };
            cpu.NMIACK_M1 = () => { };
            cpu.INTACK_M1 = () => { };
            cpu.SCANSIG = () => { };

            // No interrupts during inflate
            cpu.IFF1 = false;
            cpu.IFF2 = false;
            cpu.INT = false;
            cpu.NMI = false;
            cpu.RST = false;
            cpu.IM = 1;

            // Set up registers as inflate_call.s would before JP 0xC000:
            //   A  = src start page (0xA1)
            //   E  = dst start page (0x20)
            //   D  = saved Win 2 page (0x67 — the plugin base page)
            //   SP = 0xFFF0
            //   Return address pushed on stack
            cpu.regs.A = SRC_START_PAGE;       // 0xA1
            cpu.regs.E = DST_START_PAGE;       // 0x20
            cpu.regs.D = WC_BASE_PAGE;         // 0x67 — saved pg2
            cpu.regs.SP = 0xFFF0;
            cpu.regs.PC = 0xC000;

            // Push a sentinel return address (0x0000) so RET goes to addr 0,
            // which we detect as "inflate returned"
            cpu.regs.SP -= 2;
            mem.Write(cpu.regs.SP, 0x00);       // low byte
            mem.Write((ushort)(cpu.regs.SP + 1), 0x00);  // high byte

            cpu.Tact = 0;
            cpu.FX = CpuModeIndex.None;
            cpu.XFX = CpuModeEx.None;
            cpu.HALTED = false;

            Console.WriteLine($"PC=0x{cpu.regs.PC:X4} SP=0x{cpu.regs.SP:X4}");
            Console.WriteLine($"A=0x{cpu.regs.A:X2} D=0x{cpu.regs.D:X2} E=0x{cpu.regs.E:X2}");
            Console.WriteLine($"Return sentinel at [0x{cpu.regs.SP:X4}] = 0x0000");
        }

        static void HandlePortWrite(ushort port, byte value)
        {
            // TSConfig page ports
            if (mem.HandlePort(port, value))
            {
                // Log page changes (optional verbose)
                return;
            }

            // Debug ports
            switch (port)
            {
                case DBG_STATUS:
                    lastDbgStatus = value;
                    lastDbgCycle = cpu.Tact;
                    PrintDbgStatus(value);
                    break;

                case DBG_SRC:
                    lastDbgSrc = value;
                    lastDbgCycle = cpu.Tact;
                    break;

                case DBG_DST:
                    lastDbgDst = value;
                    lastDbgCycle = cpu.Tact;
                    break;
            }
        }

        static readonly Dictionary<byte, string> StatusNames = new()
        {
            [0x0E] = "inflate entry",
            [0x0F] = "stores done",
            [0x10] = "Win0+Win2 remapped",
            [0x11] = "Win2→PAGE_DECODE",
            [0x12] = "gzip header OK",
            [0x14] = "blocktype0 (stored)",
            [0x15] = "blocktype1 (static)",
            [0x16] = "blocktype2 (dynamic)",
            [0x17] = "Huffman tables built → decode loop",
            [0x20] = "page flush (bt0)",
            [0x21] = "page flush (decode)",
            [0x30] = "EXIT OK",
            [0x31] = "FAIL",
            [0xA6] = "bt2: HLIT/HDIST/HCLEN read",
            [0xA7] = "bt2: makeHuffmanTable(19) done",
            [0xA8] = "bt2: decodeProtoHuffman done",
            [0xA9] = "bt2: code_length_orders read → mht",
            [0xB1] = "mht: count loop done",
            [0xB2] = "mht: start codes done",
            [0xB3] = "mht: code assignment done",
            [0xBF] = "decodeProtoHuffman entry",
            [0xC0] = "dph: loop iteration",
            [0xC1] = "dph: leaf found",
            [0xC2] = "dph: special code",
        };

        static void PrintDbgStatus(byte code)
        {
            string name = StatusNames.TryGetValue(code, out var n) ? n : "???";
            string extra = "";
            // For codes that also emit SRC/DST, show them
            if (code == 0xC1 || code == 0xC2)
                extra = $"  FEAF=0x{lastDbgSrc:X2}";
            else if (code == 0x10 || code == 0x20 || code == 0x21 || code == 0x30)
                extra = $"  src_pg=0x{lastDbgSrc:X2} dst_pg=0x{lastDbgDst:X2}";

            Console.ForegroundColor = code switch
            {
                0x30 => ConsoleColor.Green,
                0x31 => ConsoleColor.Red,
                >= 0xB0 and <= 0xBF => ConsoleColor.DarkGray,  // mht internals
                >= 0xC0 and <= 0xCF => ConsoleColor.DarkCyan,  // dph internals
                _ => ConsoleColor.Yellow
            };
            Console.WriteLine($"  [{cpu.Tact,10}] DBG 0x{code:X2}: {name}{extra}");
            Console.ResetColor();
        }

        static int RunCpu()
        {
            long cycleCount = 0;

            while (!finished)
            {
                // ── Watchdog: execution outside Win3 ──
                ushort pc = cpu.regs.PC;
                if (pc < 0xC000)
                {
                    if (pc == 0x0000)
                    {
                        // Sentinel return address — inflate returned!
                        Console.ForegroundColor = ConsoleColor.Green;
                        Console.WriteLine($"\n  ✓ Inflate returned at cycle {cpu.Tact}, A=0x{cpu.regs.A:X2}");
                        Console.ResetColor();
                        if (cpu.regs.A == 0)
                            Console.WriteLine("  Result: SUCCESS (A=0, no carry)");
                        else
                            Console.WriteLine($"  Result: FAIL (A=0x{cpu.regs.A:X2})");

                        finished = true;
                        DumpState("return");
                        return cpu.regs.A == 0 ? 0 : 2;
                    }
                    else
                    {
                        Console.ForegroundColor = ConsoleColor.Red;
                        Console.WriteLine($"\n  ✗ RUNAWAY: PC=0x{pc:X4} at cycle {cpu.Tact}");
                        Console.WriteLine($"    Last DBG: 0x{lastDbgStatus:X2} at cycle {lastDbgCycle}");
                        Console.ResetColor();
                        DumpState("runaway");
                        return 3;
                    }
                }

                // ── Watchdog: cycles without debug port activity ──
                if (cpu.Tact - lastDbgCycle > watchdogLimit && lastDbgCycle > 0)
                {
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine($"\n  ✗ WATCHDOG: {cpu.Tact - lastDbgCycle} cycles since last debug output");
                    Console.WriteLine($"    Last DBG: 0x{lastDbgStatus:X2} at cycle {lastDbgCycle}");
                    Console.WriteLine($"    PC=0x{cpu.regs.PC:X4} SP=0x{cpu.regs.SP:X4}");
                    Console.ResetColor();
                    PrintCpuState();
                    if (dumpOnHang)
                        DumpState("watchdog");
                    return 4;
                }

                // ── Watchdog: max total cycles ──
                if (cpu.Tact > maxCycles)
                {
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine($"\n  ✗ MAX CYCLES ({maxCycles}) exceeded");
                    Console.ResetColor();
                    DumpState("maxcycles");
                    return 5;
                }

                // ── HALT detection ──
                if (cpu.HALTED)
                {
                    Console.ForegroundColor = ConsoleColor.Red;
                    Console.WriteLine($"\n  ✗ HALT at PC=0x{cpu.regs.PC:X4}, cycle {cpu.Tact}");
                    Console.ResetColor();
                    DumpState("halt");
                    return 6;
                }

                // ── Execute one instruction ──
                cpu.ExecCycle();
                cycleCount++;
            }

            return 0;
        }

        static void PrintCpuState()
        {
            var r = cpu.regs;
            Console.WriteLine($"    AF={r.AF:X4} BC={r.BC:X4} DE={r.DE:X4} HL={r.HL:X4}");
            Console.WriteLine($"    AF'={r._AF:X4} BC'={r._BC:X4} DE'={r._DE:X4} HL'={r._HL:X4}");
            Console.WriteLine($"    IX={r.IX:X4} IY={r.IY:X4} SP={r.SP:X4} PC={r.PC:X4}");
            Console.WriteLine($"    Win0=pg{mem.GetWindow(0):X2} Win1=pg{mem.GetWindow(1):X2} Win2=pg{mem.GetWindow(2):X2} Win3=pg{mem.GetWindow(3):X2}");

            // Disassemble a few instructions around PC
            Console.Write("    Code @PC: ");
            for (int i = 0; i < 8; i++)
                Console.Write($"{mem.Read((ushort)(r.PC + i)):X2} ");
            Console.WriteLine();

            // Stack dump
            Console.Write("    Stack: ");
            for (int i = 0; i < 8; i++)
                Console.Write($"{mem.Read((ushort)(r.SP + i)):X2} ");
            Console.WriteLine();
        }

        static void DumpState(string reason)
        {
            if (!dumpOnHang) return;

            try
            {
                Directory.CreateDirectory(dumpDir);

                // Dump inflate page (Win3)
                string inflFile = Path.Combine(dumpDir, $"{reason}_inflate_page_{INFLATE_PAGE:X2}.bin");
                File.WriteAllBytes(inflFile, mem.GetPage(INFLATE_PAGE));
                Console.WriteLine($"  Dumped: {inflFile}");

                // Dump decode buffer page
                string decFile = Path.Combine(dumpDir, $"{reason}_decode_page_{PAGE_DECODE:X2}.bin");
                File.WriteAllBytes(decFile, mem.GetPage(PAGE_DECODE));
                Console.WriteLine($"  Dumped: {decFile}");

                // Dump destination pages
                for (int p = DST_START_PAGE; p < DST_START_PAGE + 16; p++)
                {
                    byte[] pg = mem.GetPage(p);
                    bool allZero = true;
                    for (int i = 0; i < pg.Length && allZero; i++)
                        if (pg[i] != 0) allZero = false;
                    if (!allZero)
                    {
                        string dstFile = Path.Combine(dumpDir, $"{reason}_dst_page_{p:X2}.bin");
                        File.WriteAllBytes(dstFile, pg);
                    }
                }
                Console.WriteLine($"  Dumped destination pages (0x{DST_START_PAGE:X2}+)");

                // Dump Win3 area (the whole mapped page, including runtime modifications)
                string win3File = Path.Combine(dumpDir, $"{reason}_win3_mapped.bin");
                File.WriteAllBytes(win3File, mem.GetPage(mem.GetWindow(3)));
                Console.WriteLine($"  Dumped: {win3File} (Win3=page 0x{mem.GetWindow(3):X2})");

                // CPU state dump
                string stateFile = Path.Combine(dumpDir, $"{reason}_cpu_state.txt");
                using (var sw = new StreamWriter(stateFile))
                {
                    var r = cpu.regs;
                    sw.WriteLine($"Reason: {reason}");
                    sw.WriteLine($"Tact: {cpu.Tact}");
                    sw.WriteLine($"AF={r.AF:X4} BC={r.BC:X4} DE={r.DE:X4} HL={r.HL:X4}");
                    sw.WriteLine($"AF'={r._AF:X4} BC'={r._BC:X4} DE'={r._DE:X4} HL'={r._HL:X4}");
                    sw.WriteLine($"IX={r.IX:X4} IY={r.IY:X4} SP={r.SP:X4} PC={r.PC:X4}");
                    sw.WriteLine($"Win0=0x{mem.GetWindow(0):X2} Win1=0x{mem.GetWindow(1):X2} Win2=0x{mem.GetWindow(2):X2} Win3=0x{mem.GetWindow(3):X2}");
                    sw.WriteLine($"Last DBG status=0x{lastDbgStatus:X2} src=0x{lastDbgSrc:X2} dst=0x{lastDbgDst:X2}");
                    sw.WriteLine($"Last DBG at cycle {lastDbgCycle}");
                    sw.WriteLine();

                    // Disassemble around PC
                    sw.Write("Code @PC: ");
                    for (int i = 0; i < 32; i++)
                        sw.Write($"{mem.Read((ushort)(r.PC + i)):X2} ");
                    sw.WriteLine();

                    sw.Write("Stack: ");
                    for (int i = 0; i < 16; i++)
                        sw.Write($"{mem.Read((ushort)(r.SP + i)):X2} ");
                    sw.WriteLine();
                }
                Console.WriteLine($"  Dumped: {stateFile}");
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine($"  Dump error: {ex.Message}");
            }
        }

        static void AnalyzeResults()
        {
            if (refPath == null || !File.Exists(refPath))
            {
                Console.WriteLine("No ref.vgm found, skipping comparison.");
                return;
            }

            byte[] refData = File.ReadAllBytes(refPath);
            Console.WriteLine($"ref.vgm: {refData.Length} bytes");

            // Collect decompressed output from destination pages
            int refPages = (refData.Length + TsMemory.PageSize - 1) / TsMemory.PageSize;
            byte[] output = mem.ReadPages(DST_START_PAGE, refPages);

            // Compare
            int firstDiff = -1;
            int matchBytes = 0;
            int compareLen = Math.Min(refData.Length, output.Length);

            for (int i = 0; i < compareLen; i++)
            {
                if (output[i] == refData[i])
                    matchBytes++;
                else if (firstDiff < 0)
                    firstDiff = i;
            }

            if (firstDiff < 0 && matchBytes == refData.Length)
            {
                Console.ForegroundColor = ConsoleColor.Green;
                Console.WriteLine($"  ✓ PERFECT MATCH: {matchBytes} bytes identical to ref.vgm");
                Console.ResetColor();
            }
            else
            {
                Console.ForegroundColor = ConsoleColor.Red;
                Console.WriteLine($"  ✗ MISMATCH at offset 0x{firstDiff:X}");
                Console.WriteLine($"    Matched: {matchBytes}/{refData.Length} bytes ({100.0 * matchBytes / refData.Length:F1}%)");
                Console.ResetColor();

                if (firstDiff >= 0)
                {
                    Console.Write("    ref: ");
                    for (int i = Math.Max(0, firstDiff - 4); i < Math.Min(refData.Length, firstDiff + 12); i++)
                        Console.Write(i == firstDiff ? $"[{refData[i]:X2}]" : $" {refData[i]:X2} ");
                    Console.WriteLine();

                    Console.Write("    got: ");
                    for (int i = Math.Max(0, firstDiff - 4); i < Math.Min(output.Length, firstDiff + 12); i++)
                        Console.Write(i == firstDiff ? $"[{output[i]:X2}]" : $" {output[i]:X2} ");
                    Console.WriteLine();
                }

                // Save output for analysis
                try
                {
                    Directory.CreateDirectory(dumpDir);
                    string outFile = Path.Combine(dumpDir, "output.vgm");
                    byte[] trimmed = new byte[refData.Length];
                    Array.Copy(output, trimmed, Math.Min(output.Length, trimmed.Length));
                    File.WriteAllBytes(outFile, trimmed);
                    Console.WriteLine($"    Saved decompressed output: {outFile}");
                }
                catch { }
            }
        }
    }
}

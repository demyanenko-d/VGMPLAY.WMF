// WcKernel — WC (Wild Commander) API emulator for the C# debugger
// Traps CALL 0x6006 and emulates function dispatch based on A register
//
// Emulated functions (used by VGM player):
//   0x00 = MNGC_PL   — plugin page → Win3
//   0x01 = PRWOW      — draw window (stub)
//   0x02 = RRESB      — restore window (stub)
//   0x05 = GADRW      — get screen address
//   0x0E = TURBOPL    — CPU speed (stub)
//   0x0F = GEDPL      — restore display (stub)
//   0x30 = LOAD512    — load sectors from file → memory
//   0x41 = MNGCVPL    — video page → Win3
//   0x4E = MNG0_PL    — plugin page → Win0
//   0x4F = MNG8_PL    — plugin page → Win2
//   + keyboard stubs, file stubs, etc.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using ZXMAK2.Engine.Cpu;
using ZXMAK2.Engine.Cpu.Processor;

namespace InflateDebugger
{
    public class WcKernel
    {
        // ── WC Constants ─────────────────────────────────────────────

        public const ushort WC_ENTRY = 0x6006;

        // Physical page bases
        public const byte PLGPG = 0x61;   // plugin page base (page 0 = 0x61)
        public const byte TVBPG = 0x20;   // megabuffer / video page base
        public const byte MTXPG = 0x00;   // main text page

        // System variable addresses (in Win1 = 0x4000–0x7FFF)
        public const ushort SYS_PG0 = 0x6000;
        public const ushort SYS_PG4 = 0x6001;
        public const ushort SYS_PG8 = 0x6002;
        public const ushort SYS_PGC = 0x6003;
        public const ushort SYS_ABT = 0x6004;
        public const ushort SYS_ENT = 0x6005;
        public const ushort SYS_TMN = 0x6009;
        public const ushort SYS_CNFV = 0x600D;
        public const ushort SYS_HEI = 0x600E;

        // Function numbers (A register values for CALL 0x6006)
        public const byte FN_MNGC_PL   = 0x00;
        public const byte FN_PRWOW     = 0x01;
        public const byte FN_RRESB     = 0x02;
        public const byte FN_PRSRW     = 0x03;
        public const byte FN_TXTPR     = 0x04;
        public const byte FN_GADRW     = 0x05;
        public const byte FN_CURSOR    = 0x06;
        public const byte FN_CURSER    = 0x07;
        public const byte FN_YN        = 0x08;
        public const byte FN_ISTR      = 0x09;
        public const byte FN_NORK      = 0x0A;
        public const byte FN_MEZZ      = 0x0B;
        public const byte FN_SCRLWOW   = 0x0C;
        public const byte FN_PRSRW_A   = 0x0D;
        public const byte FN_TURBOPL   = 0x0E;
        public const byte FN_GEDPL     = 0x0F;

        // Keyboard: 0x10–0x2F
        public const byte FN_KEY_SPACE = 0x10;
        public const byte FN_KEY_UP    = 0x11;
        public const byte FN_KEY_DOWN  = 0x12;
        public const byte FN_KEY_LEFT  = 0x13;
        public const byte FN_KEY_RIGHT = 0x14;
        public const byte FN_KEY_TAB   = 0x15;
        public const byte FN_KEY_ENTER = 0x16;
        public const byte FN_KEY_ESC   = 0x17;
        public const byte FN_KEY_BSPC  = 0x18;
        public const byte FN_KEY_PGUP  = 0x19;
        public const byte FN_KEY_PGDN  = 0x1A;
        public const byte FN_KEY_HOME  = 0x1B;
        public const byte FN_KEY_END   = 0x1C;
        public const byte FN_KBSCN     = 0x1D;
        // F1–F10: 0x1E–0x27, ALT=0x28, SHIFT=0x29, CTRL=0x2A
        // DEL=0x2B, CAPS=0x2C, ANY=0x2D, WAITREL=0x2E, WAITANY=0x2F

        public const byte FN_LOAD512   = 0x30;
        public const byte FN_SAVE512   = 0x31;
        public const byte FN_GIPAGPL   = 0x32;
        public const byte FN_TENTRY    = 0x33;
        public const byte FN_CHTOSEP   = 0x34;
        public const byte FN_TMRKDFL   = 0x35;
        public const byte FN_ADIR      = 0x36;
        public const byte FN_STREAM    = 0x37;
        public const byte FN_FINDNEXT  = 0x38;
        public const byte FN_FENTRY    = 0x39;
        public const byte FN_LOADNONE  = 0x3A;
        public const byte FN_GFILE     = 0x3B;
        public const byte FN_GDIR      = 0x3C;
        public const byte FN_MKFILE    = 0x3D;
        public const byte FN_MKDIR     = 0x3E;
        public const byte FN_RENAME    = 0x3F;
        public const byte FN_DELFL     = 0x40;

        public const byte FN_MNGCVPL   = 0x41;
        public const byte FN_MNGV_PL   = 0x42;
        public const byte FN_GVMOD     = 0x43;
        public const byte FN_GYOFF     = 0x44;
        public const byte FN_GXOFF     = 0x45;
        public const byte FN_GVTM      = 0x46;
        public const byte FN_GVTL      = 0x47;
        public const byte FN_GVSGP     = 0x48;
        public const byte FN_MNG0VPL   = 0x49;
        public const byte FN_MNG8VPL   = 0x4A;
        public const byte FN_DMAPL     = 0x4B;
        public const byte FN_PRM_PL    = 0x4C;
        public const byte FN_INT_PL    = 0x4D;
        public const byte FN_MNG0_PL   = 0x4E;
        public const byte FN_MNG8_PL   = 0x4F;

        public const byte FN_STRSET    = 0x50;
        public const byte FN_LOAD256   = 0x51;
        public const byte FN_INT_PL_H  = 0x52;
        public const byte FN_RES_JUMP  = 0x53;
        public const byte FN_RES_CALL  = 0x54;
        public const byte FN_KEY_INS   = 0x55;

        // ── State ─────────────────────────────────────────────────────

        private readonly TsMemory _mem;
        private CpuUnit _cpu;

        // Plugin page offset within the 64-page plugin area (typically 0)
        public byte PPGP { get; set; } = 0;

        // Emulated file I/O state
        private byte[] _fileData;      // loaded file data (whole VGM/VGZ)
        private int _filePos;          // current file read position (byte offset)
        public uint FileSize => _fileData != null ? (uint)_fileData.Length : 0;

        // Disk root directory (test_data path)
        public string DiskRootPath { get; set; }

        // WC call log
        public List<WcCallRecord> CallLog { get; } = new();
        public bool LogCalls { get; set; } = true;
        public int MaxCallLog { get; set; } = 10000;

        // Saved Win0 page (P8000 saves/restores it on each WC call)
        private byte _savedWin0Page;

        // Screen height (for gadrw calculations)
        public byte ScreenHeight { get; set; } = 36;  // 90×36 (TextMode=2)
        public byte ScreenWidth { get; set; } = 90;

        // WC system page (mapped to Win1 permanently)
        public byte SysVarPage { get; set; } = 0x05;

        // Callbacks for logging
        public Action<string> Log { get; set; }

        // ── Constructor ───────────────────────────────────────────────

        public WcKernel(TsMemory mem, CpuUnit cpu)
        {
            _mem = mem;
            _cpu = cpu;
        }

        // ── File I/O emulation ────────────────────────────────────────

        /// <summary>Load a file for emulated file I/O (simulates WC file open + stream)</summary>
        public void SetFileData(byte[] data)
        {
            _fileData = data;
            _filePos = 0;
        }

        /// <summary>Load a file from disk root by name</summary>
        public bool OpenFile(string name)
        {
            if (string.IsNullOrEmpty(DiskRootPath)) return false;
            string path = Path.Combine(DiskRootPath, name);
            if (!File.Exists(path)) return false;
            _fileData = File.ReadAllBytes(path);
            _filePos = 0;
            return true;
        }

        // ── Setup system variables ────────────────────────────────────

        public void InitSysVars()
        {
            var pg = _mem.GetPage(SysVarPage);

            // PG0–PGC (0x6000–0x6003 = offset 0x2000 in Win1)
            pg[0x2000] = _mem.GetWindow(0);
            pg[0x2001] = _mem.GetWindow(1);
            pg[0x2002] = _mem.GetWindow(2);
            pg[0x2003] = _mem.GetWindow(3);

            // ABT / ENT = 0
            pg[0x2004] = 0;
            pg[0x2005] = 0;

            // JP FUN instruction at 0x6006 (we trap this, but put real bytes for reads)
            pg[0x2006] = 0xC3;  // JP
            pg[0x2007] = 0x00;
            pg[0x2008] = 0x00;

            // TMN timer
            pg[0x2009] = 0;
            pg[0x200A] = 0;

            // CNFv = 7 (VDAC2/FT812 — TSConfig default)
            pg[0x200D] = 7;

            // HEI = screen height
            pg[0x200E] = ScreenHeight;
        }

        // ── Update PGC in sys vars after page switch ──────────────────

        private void UpdateSysPGC()
        {
            _mem.GetPage(SysVarPage)[0x2003] = _mem.GetWindow(3);
        }
        private void UpdateSysPG0()
        {
            _mem.GetPage(SysVarPage)[0x2000] = _mem.GetWindow(0);
        }
        private void UpdateSysPG8()
        {
            _mem.GetPage(SysVarPage)[0x2002] = _mem.GetWindow(2);
        }

        // ── Main trap handler ─────────────────────────────────────────

        /// <summary>
        /// Check if CPU is about to execute CALL 0x6006 and intercept it.
        /// Call this BEFORE ExecCycle() when PC == address of CALL 0x6006.
        /// Returns true if the call was intercepted.
        /// </summary>
        public bool TryIntercept()
        {
            ushort pc = _cpu.regs.PC;

            // Check: is the instruction at PC a CALL 0x6006?
            // CALL nn = CD lo hi → CD 06 60
            if (_mem.Read(pc) == 0xCD &&
                _mem.Read((ushort)(pc + 1)) == 0x06 &&
                _mem.Read((ushort)(pc + 2)) == 0x60)
            {
                byte funcNum = _cpu.regs.A;
                ushort retAddr = (ushort)(pc + 3);

                // Log the call
                if (LogCalls && CallLog.Count < MaxCallLog)
                {
                    CallLog.Add(new WcCallRecord
                    {
                        Tact = _cpu.Tact,
                        PC = pc,
                        FuncNum = funcNum,
                        AF = _cpu.regs.AF,
                        BC = _cpu.regs.BC,
                        DE = _cpu.regs.DE,
                        HL = _cpu.regs.HL,
                        AF2 = _cpu.regs._AF,
                        IX = _cpu.regs.IX,
                        SP = _cpu.regs.SP
                    });
                }

                // Emulate P8000 dispatch: save Win0, set Win3=TXT
                _savedWin0Page = _mem.GetWindow(0);

                // Execute the function
                DispatchFunction(funcNum);

                // P8000 epilogue: restore Win0 only.
                // Do NOT reset Win3 — page management functions (MNGCPL, MNGCVPL)
                // explicitly set Win3 and the caller expects it to persist.
                _mem.SetWindow(0, _savedWin0Page);
                // Update sys vars to reflect actual window state
                UpdateSysPGC();
                UpdateSysPG0();

                // Set PC past the CALL instruction
                _cpu.regs.PC = retAddr;

                // Advance Tact (approximate: a typical WC call takes ~100-500 T-states)
                _cpu.Tact += 100;

                return true;
            }

            return false;
        }

        // ── Function dispatcher ───────────────────────────────────────

        private void DispatchFunction(byte funcNum)
        {
            switch (funcNum)
            {
                // ── Page management ──
                case FN_MNGC_PL:   Fn_MngcPl();    break;
                case FN_MNG0_PL:   Fn_Mng0Pl();    break;
                case FN_MNG8_PL:   Fn_Mng8Pl();    break;
                case FN_MNGCVPL:   Fn_Mngcvpl();   break;
                case FN_MNG0VPL:   Fn_Mng0vpl();   break;
                case FN_MNG8VPL:   Fn_Mng8vpl();   break;

                // ── UI (stubs) ──
                case FN_GEDPL:     Fn_Gedpl();      break;
                case FN_PRWOW:     Fn_Prwow();      break;
                case FN_RRESB:     Fn_Rresb();      break;
                case FN_GADRW:     Fn_Gadrw();      break;
                case FN_PRSRW:     Fn_Stub("PRSRW");  break;
                case FN_PRSRW_A:   Fn_Stub("PRSRW_A");break;
                case FN_TXTPR:     Fn_Stub("TXTPR");   break;
                case FN_SCRLWOW:   Fn_Stub("SCRLWOW"); break;
                case FN_CURSOR:    Fn_Stub("CURSOR");  break;
                case FN_CURSER:    Fn_Stub("CURSER");  break;
                case FN_YN:        Fn_Stub("YN");      break;
                case FN_ISTR:      Fn_Stub("ISTR");    break;
                case FN_NORK:      Fn_Stub("NORK");    break;
                case FN_MEZZ:      Fn_Stub("MEZZ");    break;

                // ── Turbo / misc ──
                case FN_TURBOPL:   Fn_Turbopl();    break;
                case FN_PRM_PL:    Fn_PrmPl();      break;
                case FN_INT_PL:    Fn_Stub("INT_PL");  break;
                case FN_INT_PL_H:  Fn_Stub("INT_PL_H");break;
                case FN_STRSET:    Fn_Strset();     break;
                case FN_DMAPL:     Fn_Stub("DMAPL");   break;

                // ── Keyboard (all return 0 = not pressed) ──
                case >= FN_KEY_SPACE and <= 0x2F:
                    Fn_KeyStub(funcNum);
                    break;

                // ── File I/O ──
                case FN_LOAD512:   Fn_Load512();    break;
                case FN_LOAD256:   Fn_Load256();    break;
                case FN_SAVE512:   Fn_Stub("SAVE512"); break;
                case FN_GIPAGPL:   Fn_Gipagpl();    break;
                case FN_STREAM:    Fn_Stream();     break;
                case FN_FENTRY:    Fn_Fentry();     break;
                case FN_GFILE:     Fn_Gfile();      break;
                case FN_ADIR:      Fn_Stub("ADIR");    break;
                case FN_FINDNEXT:  Fn_Stub("FINDNEXT");break;
                case FN_TENTRY:    Fn_Stub("TENTRY");  break;
                case FN_GDIR:      Fn_Stub("GDIR");    break;
                case FN_LOADNONE:  Fn_Loadnone();   break;

                // ── Graphics (stubs) ──
                case FN_MNGV_PL:   Fn_Stub("MNGV_PL"); break;
                case FN_GVMOD:     Fn_Stub("GVMOD");    break;
                case FN_GYOFF:     Fn_Stub("GYOFF");    break;
                case FN_GXOFF:     Fn_Stub("GXOFF");    break;

                // ── Resident calls ──
                case FN_RES_JUMP:  Fn_Stub("RES_JUMP"); break;
                case FN_RES_CALL:  Fn_Stub("RES_CALL"); break;

                default:
                    Log?.Invoke($"  WC UNKNOWN function 0x{funcNum:X2} at tact={_cpu.Tact}");
                    break;
            }
        }

        // ═══════════════════════════════════════════════════════════════
        // Page management functions
        // ═══════════════════════════════════════════════════════════════

        /// <summary>0x00 MNGC_PL: plugin page A' → Win3. phys = A' + PLGPG + PPGP</summary>
        private void Fn_MngcPl()
        {
            byte pageArg = (byte)(_cpu.regs._AF >> 8);  // A' register

            if (pageArg == 0xFE)
            {
                // 0xFE = first text screen
                _mem.SetWindow(3, MTXPG);
            }
            else if (pageArg == 0xFF)
            {
                // 0xFF = font page
                _mem.SetWindow(3, 0x01);
            }
            else if (pageArg < 64)
            {
                byte phys = (byte)(pageArg + PLGPG + PPGP);
                _mem.SetWindow(3, phys);
            }
            // else >= 64: NOP (return without changing)

            UpdateSysPGC();
        }

        /// <summary>0x4E MNG0_PL: plugin page A' → Win0. phys = A' + PLGPG + PPGP</summary>
        private void Fn_Mng0Pl()
        {
            byte pageArg = (byte)(_cpu.regs._AF >> 8);
            if (pageArg < 64)
            {
                byte phys = (byte)(pageArg + PLGPG + PPGP);
                _mem.SetWindow(0, phys);
                UpdateSysPG0();
            }
        }

        /// <summary>0x4F MNG8_PL: plugin page A' → Win2. phys = A' + PLGPG + PPGP</summary>
        private void Fn_Mng8Pl()
        {
            byte pageArg = (byte)(_cpu.regs._AF >> 8);
            if (pageArg < 64)
            {
                byte phys = (byte)(pageArg + PLGPG + PPGP);
                _mem.SetWindow(2, phys);
                UpdateSysPG8();
            }
        }

        /// <summary>0x41 MNGCVPL: video page A' → Win3. phys = A' + TVBPG(0x20)</summary>
        private void Fn_Mngcvpl()
        {
            byte vpage = (byte)(_cpu.regs._AF >> 8);
            if (vpage < 64)
            {
                byte phys = (byte)(vpage + TVBPG);
                _mem.SetWindow(3, phys);
            }
            UpdateSysPGC();
        }

        /// <summary>0x49 MNG0VPL: video page A' → Win0</summary>
        private void Fn_Mng0vpl()
        {
            byte vpage = (byte)(_cpu.regs._AF >> 8);
            if (vpage < 64)
            {
                byte phys = (byte)(vpage + TVBPG);
                _mem.SetWindow(0, phys);
                UpdateSysPG0();
            }
        }

        /// <summary>0x4A MNG8VPL: video page A' → Win2</summary>
        private void Fn_Mng8vpl()
        {
            byte vpage = (byte)(_cpu.regs._AF >> 8);
            if (vpage < 64)
            {
                byte phys = (byte)(vpage + TVBPG);
                _mem.SetWindow(2, phys);
                UpdateSysPG8();
            }
        }

        // ═══════════════════════════════════════════════════════════════
        // UI functions (mostly stubs for text-mode UI)
        // ═══════════════════════════════════════════════════════════════

        /// <summary>0x0F GEDPL: restore WC display (stub — sets Win3 to TXT)</summary>
        private void Fn_Gedpl()
        {
            // Real WC: restores palette, attributes, text mode
            // Also trashes 0x1000–0x1447 in Win0 — we emulate that
            _mem.SetWindow(3, MTXPG);
            UpdateSysPGC();
            Log?.Invoke("  WC GEDPL (stub)");
        }

        /// <summary>0x01 PRWOW: draw window (stub)</summary>
        private void Fn_Prwow()
        {
            // IX = win ptr (from HL in sdcccall)
            _mem.SetWindow(3, MTXPG);
            UpdateSysPGC();
        }

        /// <summary>0x02 RRESB: restore window background (stub)</summary>
        private void Fn_Rresb()
        {
            _mem.SetWindow(3, MTXPG);
            UpdateSysPGC();
        }

        /// <summary>0x05 GADRW: get screen address for (y,x) in window</summary>
        private void Fn_Gadrw()
        {
            // In wc_api.s: IX=win, D=y, E=x
            // Returns DE = screen address
            // Text screen at 0xC000, each row = ScreenWidth bytes
            // Window content starts at win.x + win.y * ScreenWidth + 0xC000
            // We compute: addr = 0xC000 + (win_y + y) * ScreenWidth + (win_x + x)

            ushort winPtr = _cpu.regs.IX;
            byte wy = _mem.Read((ushort)(winPtr + 3));  // win.y at +3
            byte wx = _mem.Read((ushort)(winPtr + 2));  // win.x at +2
            byte y = _cpu.regs.D;
            byte x = _cpu.regs.E;

            ushort addr = (ushort)(0xC000 + (wy + y) * ScreenWidth + (wx + x));
            _cpu.regs.DE = addr;

            // Also need to set Win3 to TXT
            _mem.SetWindow(3, MTXPG);
            UpdateSysPGC();
        }

        /// <summary>0x0E TURBOPL: set CPU/AY speed via SYSCONF port</summary>
        private void Fn_Turbopl()
        {
            // B = mode (0=CPU, 1=AY, 0xFF=restore), C = param
            byte mode  = (byte)(_cpu.regs.BC >> 8);   // B
            byte param = (byte)(_cpu.regs.BC & 0xFF);  // C

            if (mode == 0x00)  // WC_TURBO_CPU: set CPU frequency
            {
                // param: 0=3.5, 1=7, 2=14, 3=28
                // Write turbo bits to SYSCONF port so PluginRunner picks it up
                byte sysconf = (byte)(param & 0x03);
                _cpu.WRPORT(0x20AF, sysconf);
            }
            // mode 1 (AY freq) — not relevant for emulation
            // mode 0xFF (restore) — restore to 3.5 MHz
            else if (mode == 0xFF)
            {
                _cpu.WRPORT(0x20AF, 0x00);  // restore to 3.5 MHz
            }
        }

        /// <summary>0x4C PRM_PL: get INI parameter (stub — return 0xFF = not set)</summary>
        private void Fn_PrmPl()
        {
            _cpu.regs.A = 0xFF;
        }

        /// <summary>0x50 STRSET: memset(ptr, len, char)</summary>
        private void Fn_Strset()
        {
            // In wc_api.s: HL=dst, DE=len, A'=char
            // We actually don't need real WC for this, but the plugin calls it
            ushort dst = _cpu.regs.HL;
            ushort len = _cpu.regs.DE;
            byte ch = (byte)(_cpu.regs._AF >> 8);

            for (int i = 0; i < len; i++)
                _mem.Write((ushort)(dst + i), ch);
        }

        // ═══════════════════════════════════════════════════════════════
        // Keyboard stubs (all return A=0 = not pressed)
        // ═══════════════════════════════════════════════════════════════

        private void Fn_KeyStub(byte funcNum)
        {
            _cpu.regs.A = 0;  // not pressed
            // Clear Z flag by default (A=0 → Z=1 → key not pressed)
        }

        // ═══════════════════════════════════════════════════════════════
        // File I/O emulation
        // ═══════════════════════════════════════════════════════════════

        /// <summary>0x30 LOAD512: load blocks×512 bytes from file → memory at dest</summary>
        private void Fn_Load512()
        {
            // wc_api.s _wc_load512: HL=dest, B=blocks (from stack via frame pointer)
            // WC API #0x30: HL=dest, B=blocks → A=0 ok / 0x0F EOF
            // Returns: A = status, HL = next address after loaded data

            ushort loadDest = _cpu.regs.HL;
            byte blocks = _cpu.regs.B;
            int bytesToLoad = blocks * 512;

            if (_fileData == null)
            {
                Log?.Invoke("  WC LOAD512: no file data!");
                _cpu.regs.A = 0xFF;  // error
                return;
            }

            int remaining = _fileData.Length - _filePos;
            int actual = Math.Min(bytesToLoad, remaining);
            bool isEof = (actual < bytesToLoad) || (_filePos + actual >= _fileData.Length);

            // Write data into memory at loadDest (using Win3 map)
            for (int i = 0; i < actual; i++)
            {
                _mem.Write((ushort)(loadDest + i), _fileData[_filePos + i]);
            }
            _filePos += actual;

            // Update HL to point past the loaded data
            _cpu.regs.HL = (ushort)(loadDest + actual);

            _cpu.regs.A = isEof ? (byte)0x0F : (byte)0; // 0x0F = WC_EOF

            Log?.Invoke($"  LOAD512: dest=0x{loadDest:X4} blocks={blocks} bytes={actual} eof={isEof} filePos={_filePos}/{_fileData.Length}");
        }

        /// <summary>0x51 LOAD256: load blocks×256 bytes</summary>
        private void Fn_Load256()
        {
            // Same pattern as LOAD512 but 256-byte blocks: HL=dest, B=blocks
            ushort loadDest = _cpu.regs.HL;
            byte blocks = _cpu.regs.B;
            int bytesToLoad = blocks * 256;

            if (_fileData == null) { _cpu.regs.A = 0xFF; return; }

            int remaining = _fileData.Length - _filePos;
            int actual = Math.Min(bytesToLoad, remaining);
            bool isEof = actual < bytesToLoad || _filePos + actual >= _fileData.Length;

            for (int i = 0; i < actual; i++)
                _mem.Write((ushort)(loadDest + i), _fileData[_filePos + i]);
            _filePos += actual;

            _cpu.regs.HL = (ushort)(loadDest + actual);
            _cpu.regs.A = isEof ? (byte)0x0F : (byte)0;

            Log?.Invoke($"  LOAD256: dest=0x{loadDest:X4} blocks={blocks} bytes={actual} eof={isEof}");
        }

        /// <summary>0x32 GIPAGPL: seek file to beginning</summary>
        private void Fn_Gipagpl()
        {
            _filePos = 0;
        }

        /// <summary>0x37 STREAM: open/switch file stream (stub — just log)</summary>
        private void Fn_Stream()
        {
            // D = mode (0xFF=root, 0xFE=clone, 0xFD=wcdir, 0/1=stream#)
            Log?.Invoke($"  WC STREAM mode=0x{_cpu.regs.D:X2}");
        }

        /// <summary>0x39 FENTRY: find file by name</summary>
        private void Fn_Fentry()
        {
            // HL = addr of [flag_byte][name_string][0]
            ushort namePtr = _cpu.regs.HL;
            byte flag = _mem.Read(namePtr);
            namePtr++;

            // Read filename
            var sb = new StringBuilder();
            for (int i = 0; i < 255; i++)
            {
                byte ch = _mem.Read((ushort)(namePtr + i));
                if (ch == 0) break;
                sb.Append((char)ch);
            }
            string fileName = sb.ToString();
            Log?.Invoke($"  WC FENTRY: \"{fileName}\" flag=0x{flag:X2}");

            // Try to find the file on disk root
            if (!string.IsNullOrEmpty(DiskRootPath))
            {
                string path = Path.Combine(DiskRootPath, fileName);
                if (File.Exists(path))
                {
                    _cpu.regs.A = 1;  // found (non-zero)
                    // Set file size in DE:HL
                    long fsize = new FileInfo(path).Length;
                    _cpu.regs.HL = (ushort)(fsize & 0xFFFF);
                    _cpu.regs.DE = (ushort)((fsize >> 16) & 0xFFFF);

                    // Store path for subsequent GFILE + LOAD512
                    _pendingFileName = path;
                    return;
                }
            }

            _cpu.regs.A = 0;  // not found
        }

        private string _pendingFileName;

        /// <summary>0x3B GFILE: open found file for reading</summary>
        private void Fn_Gfile()
        {
            if (!string.IsNullOrEmpty(_pendingFileName) && File.Exists(_pendingFileName))
            {
                _fileData = File.ReadAllBytes(_pendingFileName);
                _filePos = 0;
                Log?.Invoke($"  WC GFILE: opened \"{_pendingFileName}\" ({_fileData.Length} bytes)");
            }
            else
            {
                Log?.Invoke("  WC GFILE: no pending file");
            }
        }

        /// <summary>0x3A LOADNONE: skip N sectors in stream</summary>
        private void Fn_Loadnone()
        {
            byte sectors = (byte)(_cpu.regs._AF >> 8);
            int skip = sectors * 512;
            if (_fileData != null)
                _filePos = Math.Min(_filePos + skip, _fileData.Length);
        }

        // ═══════════════════════════════════════════════════════════════
        // Stub for unimplemented functions
        // ═══════════════════════════════════════════════════════════════

        private void Fn_Stub(string name)
        {
            // Most stubs just need Win3 = TXT (already done in P8000 epilogue)
        }

        // ═══════════════════════════════════════════════════════════════
        // Call log
        // ═══════════════════════════════════════════════════════════════

        public string DumpCallLog(int last = 50)
        {
            var sb = new StringBuilder();
            sb.AppendLine($"=== WC Call Log (last {last} of {CallLog.Count}) ===");
            int start = Math.Max(0, CallLog.Count - last);
            for (int i = start; i < CallLog.Count; i++)
            {
                var c = CallLog[i];
                sb.AppendFormat("[{0,10}] PC={1:X4} FN=0x{2:X2} ({3}) AF={4:X4} BC={5:X4} DE={6:X4} HL={7:X4} IX={8:X4}",
                    c.Tact, c.PC, c.FuncNum, GetFuncName(c.FuncNum),
                    c.AF, c.BC, c.DE, c.HL, c.IX);
                sb.AppendLine();
            }
            return sb.ToString();
        }

        public static string GetFuncName(byte fn) => fn switch
        {
            FN_MNGC_PL => "MNGC_PL",
            FN_PRWOW   => "PRWOW",
            FN_RRESB   => "RRESB",
            FN_PRSRW   => "PRSRW",
            FN_TXTPR   => "TXTPR",
            FN_GADRW   => "GADRW",
            FN_TURBOPL => "TURBOPL",
            FN_GEDPL   => "GEDPL",
            FN_LOAD512 => "LOAD512",
            FN_LOAD256 => "LOAD256",
            FN_GIPAGPL => "GIPAGPL",
            FN_STREAM  => "STREAM",
            FN_FENTRY  => "FENTRY",
            FN_GFILE   => "GFILE",
            FN_MNGCVPL => "MNGCVPL",
            FN_MNG0_PL => "MNG0_PL",
            FN_MNG8_PL => "MNG8_PL",
            FN_MNG0VPL => "MNG0VPL",
            FN_MNG8VPL => "MNG8VPL",
            FN_STRSET  => "STRSET",
            FN_PRM_PL  => "PRM_PL",
            FN_ADIR    => "ADIR",
            FN_LOADNONE=> "LOADNONE",
            FN_SAVE512 => "SAVE512",
            FN_DMAPL   => "DMAPL",
            >= FN_KEY_SPACE and <= 0x2F => $"KEY_{fn - FN_KEY_SPACE:X2}",
            _ => $"?{fn:X2}"
        };
    }

    // ── WC call record ────────────────────────────────────────────────

    public struct WcCallRecord
    {
        public long   Tact;
        public ushort PC;
        public byte   FuncNum;
        public ushort AF, BC, DE, HL;
        public ushort AF2;
        public ushort IX, SP;
    }
}

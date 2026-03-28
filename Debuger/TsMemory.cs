// TsMemory — TSConfig 256-page × 16KB paged memory with page protection
// Enhanced: write protection, dirty tracking, violation logging
using System;
using System.Collections.Generic;

namespace InflateDebugger
{
    public class TsMemory
    {
        public const int PageSize = 0x4000;   // 16KB
        public const int TotalPages = 256;    // 4 MB

        // Physical memory: 256 pages × 16KB
        private readonly byte[][] _pages;

        // Window → physical page mapping
        // Win0 = 0x0000–0x3FFF, Win1 = 0x4000–0x7FFF,
        // Win2 = 0x8000–0xBFFF, Win3 = 0xC000–0xFFFF
        private readonly byte[] _winPage = new byte[4];

        // TSConfig page ports
        public const ushort PORT_W0 = 0x10AF;
        public const ushort PORT_W1 = 0x11AF;
        public const ushort PORT_W2 = 0x12AF;
        public const ushort PORT_W3 = 0x13AF;

        // ── Page protection ──────────────────────────────────────────

        /// <summary>Set of physical page numbers that are write-protected</summary>
        private readonly HashSet<int> _protectedPages = new();

        /// <summary>Pages that have been written to (dirty tracking)</summary>
        private readonly HashSet<int> _dirtyPages = new();

        /// <summary>Protection violation log</summary>
        public List<ProtectionViolation> Violations { get; } = new();

        /// <summary>Max violations to log before stopping (0 = unlimited)</summary>
        public int MaxViolations { get; set; } = 100;

        /// <summary>If true, throw on protection violation</summary>
        public bool ThrowOnViolation { get; set; } = false;

        /// <summary>Callback invoked on every write. Args: z80addr, page, offset, value</summary>
        public Action<ushort, byte, int, byte> OnWrite { get; set; }

        /// <summary>Callback invoked on protection violation</summary>
        public Action<ProtectionViolation> OnViolation { get; set; }

        public TsMemory()
        {
            _pages = new byte[TotalPages][];
            for (int i = 0; i < TotalPages; i++)
                _pages[i] = new byte[PageSize];
        }

        // ── Page protection API ──────────────────────────────────────

        public void ProtectPage(int page) => _protectedPages.Add(page);
        public void UnprotectPage(int page) => _protectedPages.Remove(page);
        public void ProtectRange(int startPage, int count)
        {
            for (int i = 0; i < count; i++) _protectedPages.Add(startPage + i);
        }
        public void UnprotectRange(int startPage, int count)
        {
            for (int i = 0; i < count; i++) _protectedPages.Remove(startPage + i);
        }
        public bool IsProtected(int page) => _protectedPages.Contains(page);
        public void ClearProtection() => _protectedPages.Clear();

        public IReadOnlyCollection<int> DirtyPages => _dirtyPages;
        public void ClearDirtyTracking() => _dirtyPages.Clear();

        // ── Window management ────────────────────────────────────────

        /// <summary>Set window mapping (0–3) to physical page</summary>
        public void SetWindow(int win, byte page)
        {
            _winPage[win] = page;
        }

        public byte GetWindow(int win) => _winPage[win];

        // ── Memory access ────────────────────────────────────────────

        /// <summary>Read byte at 16-bit Z80 address through page windows</summary>
        public byte Read(ushort addr)
        {
            int win = addr >> 14;          // 0–3
            int offset = addr & 0x3FFF;
            return _pages[_winPage[win]][offset];
        }

        /// <summary>Write byte at 16-bit Z80 address (with protection check)</summary>
        public void Write(ushort addr, byte val)
        {
            int win = addr >> 14;
            int offset = addr & 0x3FFF;
            byte page = _winPage[win];

            _dirtyPages.Add(page);
            OnWrite?.Invoke(addr, page, offset, val);

            if (_protectedPages.Contains(page))
            {
                var v = new ProtectionViolation
                {
                    Z80Addr = addr,
                    PhysPage = page,
                    Offset = offset,
                    Value = val,
                    OldValue = _pages[page][offset],
                    Window = win
                };
                if (Violations.Count < MaxViolations || MaxViolations == 0)
                    Violations.Add(v);
                OnViolation?.Invoke(v);
                if (ThrowOnViolation)
                    throw new ProtectionViolationException(v);
            }

            _pages[page][offset] = val;
        }

        /// <summary>Direct write bypassing protection (for initial data loading)</summary>
        public void WriteUnprotected(ushort addr, byte val)
        {
            int win = addr >> 14;
            int offset = addr & 0x3FFF;
            _pages[_winPage[win]][offset] = val;
        }

        /// <summary>Direct access to a physical page</summary>
        public byte[] GetPage(int page) => _pages[page];

        /// <summary>Load data into physical pages starting at startPage</summary>
        public int LoadToPages(byte[] data, int startPage)
        {
            int pagesNeeded = (data.Length + PageSize - 1) / PageSize;
            for (int p = 0; p < pagesNeeded; p++)
            {
                int srcOff = p * PageSize;
                int len = Math.Min(PageSize, data.Length - srcOff);
                Array.Copy(data, srcOff, _pages[startPage + p], 0, len);
            }
            return pagesNeeded;
        }

        /// <summary>Handle OUT to TSConfig page port</summary>
        public bool HandlePort(ushort port, byte value)
        {
            switch (port)
            {
                case PORT_W0: SetWindow(0, value); return true;
                case PORT_W1: SetWindow(1, value); return true;
                case PORT_W2: SetWindow(2, value); return true;
                case PORT_W3: SetWindow(3, value); return true;
                default: return false;
            }
        }

        /// <summary>Read all output pages (destination) into a flat array</summary>
        public byte[] ReadPages(int startPage, int count)
        {
            byte[] result = new byte[count * PageSize];
            for (int p = 0; p < count; p++)
                Array.Copy(_pages[startPage + p], 0, result, p * PageSize, PageSize);
            return result;
        }

        /// <summary>Update WC system variable area (0x6000–0x600E)</summary>
        public void UpdateSysVars()
        {
            byte sysPage = _winPage[1];
            _pages[sysPage][0x2000] = _winPage[0];  // 0x6000 = PG0
            _pages[sysPage][0x2001] = _winPage[1];  // 0x6001 = PG4
            _pages[sysPage][0x2002] = _winPage[2];  // 0x6002 = PG8
            _pages[sysPage][0x2003] = _winPage[3];  // 0x6003 = PGC
        }

        /// <summary>Dump 16 bytes around an address for debugging</summary>
        public string HexDump(ushort addr, int before = 8, int after = 8)
        {
            int start = Math.Max(0, addr - before);
            int end = Math.Min(0xFFFF, addr + after);
            var sb = new System.Text.StringBuilder();
            for (int a = start; a <= end; a++)
            {
                if (a == addr) sb.Append('[');
                sb.AppendFormat("{0:X2}", Read((ushort)a));
                if (a == addr) sb.Append(']');
                else sb.Append(' ');
            }
            return sb.ToString();
        }
    }

    // ── Protection violation types ───────────────────────────────────

    public struct ProtectionViolation
    {
        public ushort Z80Addr;
        public byte   PhysPage;
        public int    Offset;
        public byte   Value;
        public byte   OldValue;
        public int    Window;

        public override string ToString() =>
            $"PROTECT VIOLATION: [{Z80Addr:X4}] pg={PhysPage:X2} off={Offset:X4} " +
            $"old={OldValue:X2} new={Value:X2} win={Window}";
    }

    public class ProtectionViolationException : Exception
    {
        public ProtectionViolation Violation { get; }
        public ProtectionViolationException(ProtectionViolation v)
            : base(v.ToString()) { Violation = v; }
    }
}

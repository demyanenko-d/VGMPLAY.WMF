// TSConfig memory manager for inflate debugger
// Emulates 256 pages × 16KB with 4 windows (Win0–Win3)
using System;

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

        // Simulated WC system variables area (in page mapped to Win1 normally)
        // 0x6000 = Win0 page, 0x6001 = Win1, 0x6002 = Win2, 0x6003 = Win3
        // We store these in the actual mapped memory
        public byte SysVarWin0Page { get; set; }
        public byte SysVarWin1Page { get; set; }
        public byte SysVarWin2Page { get; set; }
        public byte SysVarWin3Page { get; set; }

        public TsMemory()
        {
            _pages = new byte[TotalPages][];
            for (int i = 0; i < TotalPages; i++)
                _pages[i] = new byte[PageSize];
        }

        /// <summary>Set window mapping (0–3) to physical page</summary>
        public void SetWindow(int win, byte page)
        {
            _winPage[win] = page;
        }

        public byte GetWindow(int win) => _winPage[win];

        /// <summary>Read byte at 16-bit Z80 address through page windows</summary>
        public byte Read(ushort addr)
        {
            int win = addr >> 14;          // 0–3
            int offset = addr & 0x3FFF;
            return _pages[_winPage[win]][offset];
        }

        /// <summary>Write byte at 16-bit Z80 address through page windows</summary>
        public void Write(ushort addr, byte val)
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
}

// CpuTrace — ring buffer storing last N executed instructions with register snapshots
// Used for crash/corruption analysis in the WC emulator
using System;
using System.Text;

namespace InflateDebugger
{
    /// <summary>Snapshot of CPU state at one instruction</summary>
    public struct TraceEntry
    {
        public long   Tact;
        public ushort PC;
        public byte   Opcode;        // first byte of the instruction
        public ushort AF, BC, DE, HL;
        public ushort AF2, BC2, DE2, HL2;  // shadow regs
        public ushort IX, IY, SP;
        public byte   Win0, Win1, Win2, Win3;  // page window mapping

        // Optional: what was written (0xFFFF = no write)
        public ushort WriteAddr;
        public byte   WriteVal;

        // WC API trap info (0xFF = not a WC call)
        public byte   WcFuncNum;

        public override string ToString()
        {
            var sb = new StringBuilder();
            sb.AppendFormat("[{0,10}] PC={1:X4} op={2:X2}", Tact, PC, Opcode);
            sb.AppendFormat(" AF={0:X4} BC={1:X4} DE={2:X4} HL={3:X4}", AF, BC, DE, HL);
            sb.AppendFormat(" IX={0:X4} SP={1:X4}", IX, SP);
            sb.AppendFormat(" W[{0:X2},{1:X2},{2:X2},{3:X2}]", Win0, Win1, Win2, Win3);
            if (WcFuncNum != 0xFF)
                sb.AppendFormat(" WC_CALL=0x{0:X2}", WcFuncNum);
            if (WriteAddr != 0xFFFF)
                sb.AppendFormat(" WR[{0:X4}]={1:X2}", WriteAddr, WriteVal);
            return sb.ToString();
        }
    }

    /// <summary>Fixed-size ring buffer of CPU trace entries</summary>
    public class CpuTrace
    {
        private readonly TraceEntry[] _buf;
        private readonly int _capacity;
        private int _head;   // next write position
        private int _count;  // total entries stored (capped at capacity)

        public CpuTrace(int capacity = 200)
        {
            _capacity = capacity;
            _buf = new TraceEntry[capacity];
            _head = 0;
            _count = 0;
        }

        public int Count => _count;
        public int Capacity => _capacity;

        /// <summary>Record a new trace entry (overwrites oldest when full)</summary>
        public void Record(ref TraceEntry entry)
        {
            _buf[_head] = entry;
            _head = (_head + 1) % _capacity;
            if (_count < _capacity) _count++;
        }

        /// <summary>Get entry by age: 0 = most recent, 1 = one before, etc.</summary>
        public TraceEntry Get(int age)
        {
            if (age < 0 || age >= _count)
                throw new IndexOutOfRangeException($"age={age}, count={_count}");
            int idx = (_head - 1 - age + _capacity * 2) % _capacity;
            return _buf[idx];
        }

        /// <summary>Dump the last N entries (most recent first) to a string</summary>
        public string Dump(int maxEntries = 0)
        {
            if (maxEntries <= 0) maxEntries = _count;
            int n = Math.Min(maxEntries, _count);
            var sb = new StringBuilder();
            sb.AppendLine($"=== CPU Trace (last {n} of {_count}) ===");
            // Print oldest-first for readability
            for (int i = n - 1; i >= 0; i--)
                sb.AppendLine(Get(i).ToString());
            return sb.ToString();
        }

        /// <summary>Dump trace to file</summary>
        public void DumpToFile(string path, int maxEntries = 0)
        {
            System.IO.File.WriteAllText(path, Dump(maxEntries));
        }

        /// <summary>Clear all entries</summary>
        public void Clear()
        {
            _head = 0;
            _count = 0;
        }
    }
}

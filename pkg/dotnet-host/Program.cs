/*
 * MoonlightHost - Tizen .NET host for the dev-cert path.
 *
 * On a retail (unrooted) Samsung TV only Web (.wgt) and .NET (.tpk) apps install, so
 * the installable unit is a dev-signed .NET .tpk; our native streaming engine ships
 * inside it as libmlengine.so and is driven over the stable C ABI (mlengine.h).
 *
 * The engine is loaded from memory rather than from a file in the app folder: we
 * read libmlengine.so's bytes and load them from an anonymous in-memory fd
 * (memfd_create -> write -> dlopen /proc/self/fd/N). This is also how a downloaded
 * engine update is applied without rewriting the app folder. Only OUR .so comes from
 * memory; its dependencies (esplusplayer, ssl, curl, ecore...) resolve normally from
 * the TV's /usr/lib.
 *
 * The host is a UI app with no window (nodisplay), so the web UI, which is another
 * package, can start it. It keeps running in the background. It serves the UI
 * command port "moonlight" (see
 * pkg/ui/PROTOCOL.md): commands from the web UI are run on the engine and answered
 * on the UI port "moonlightui".
 */
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Runtime.InteropServices;
using Tizen.Applications;

namespace MoonlightHost
{
    // Verbose host trace: compiled out of release builds ([Conditional] drops the calls).
    // Host log: dlog tag MLHOST as before, plus the on-demand forward to the UI console.
    internal static class Log
    {
        [System.Diagnostics.Conditional("DEBUG")]
        public static void Trace(string m) { Tizen.Log.Debug("MLHOST", m); UiLog.Add("MLHOST", m); }
        public static void Info(string m)  { Tizen.Log.Info("MLHOST", m);  UiLog.Add("MLHOST", m); }
        public static void Error(string m) { Tizen.Log.Error("MLHOST", m); UiLog.Add("MLHOST", "ERROR " + m); }
    }

    // On-demand forward of host/engine log lines to the UI console (mlbridge.js prints
    // them), so one web console (for example Apps2Samsung's) shows every layer. Off by
    // default; the UI turns it on with the setLog command. The host keeps its last
    // lines so the start-up (engine load) is visible when it is turned on later.
    internal static class UiLog
    {
#if DEBUG
        private const int Keep = 1000, MaxLine = 1000;   // room for the start-up diagnostics (Diag.cs)
#else
        private const int Keep = 100, MaxLine = 1000;    // one message port message is ~4 KB max
#endif
        private static readonly Queue<KeyValuePair<string, string>> s_recent = new Queue<KeyValuePair<string, string>>();
        private static volatile bool s_on;
        private static readonly Engine.LogCbDelegate s_engineCb = OnEngineLine;   // kept alive for native code

        public static void Add(string tag, string line)
        {
            if (line.Length > MaxLine) line = line.Substring(0, MaxLine) + " [cut]";
            lock (s_recent)
            {
                if (tag == "MLHOST") { s_recent.Enqueue(new KeyValuePair<string, string>(tag, line)); if (s_recent.Count > Keep) s_recent.Dequeue(); }
                if (s_on) CommandPort.SendLog(tag, line);
            }
        }

        public static void Enable(bool on)
        {
            lock (s_recent)
            {
                s_on = on;
                if (on) foreach (var kv in s_recent) CommandPort.SendLog(kv.Key, kv.Value);
            }
            try { if (Engine.SetLogCb != null) Engine.SetLogCb(on ? s_engineCb : null); }
            catch (Exception ex) { Tizen.Log.Error("MLHOST", "engine log forward failed: " + ex.Message); }
            Tizen.Log.Info("MLHOST", "UI log forward " + (on ? "on" : "off"));
        }

        private static void OnEngineLine(IntPtr line)
        {
            try { Add("MLENGINE", Marshal.PtrToStringAnsi(line) ?? ""); } catch { }   // never throw into native code
        }
    }

    // Loads a shared object from memory (lets downloaded engine updates apply
    // without rewriting the app folder).
    internal static class MemLoader
    {
        private const uint MFD_CLOEXEC = 0x0001u;
        private const int  RTLD_NOW    = 0x0002;
        private const int  RTLD_GLOBAL = 0x0100;
        // Tizen 4/5 libc does NOT export memfd_create, so create the anonymous fd by
        // raw syscall. ARM (32-bit) __NR_memfd_create = 385.
        private const int  SYS_memfd_create_arm = 385;

        [DllImport("libc", EntryPoint = "syscall", SetLastError = true)]
        // number is C long = 32-bit on ARM; a C# long (64-bit) shifts name/flags into the wrong registers -> EINVAL.
        private static extern int syscall_memfd(int number, string name, uint flags);
        [DllImport("libc", SetLastError = true)]  private static extern IntPtr write(int fd, IntPtr buf, UIntPtr count);
        [DllImport("libc", SetLastError = true)]  private static extern IntPtr mmap(IntPtr addr, UIntPtr length, int prot, int flags, int fd, IntPtr offset);
        [DllImport("libc", SetLastError = true)]  private static extern int    mprotect(IntPtr addr, UIntPtr length, int prot);
        [DllImport("libdl.so.2", SetLastError = true)]  private static extern IntPtr dlopen(string filename, int flags);
        [DllImport("libdl.so.2")]                       public  static extern IntPtr dlsym(IntPtr handle, string symbol);
        [DllImport("libdl.so.2")]                       private static extern IntPtr dlerror();

        // An anonymous fd holding the bytes of so (also used by Diag.cs).
        public static int CreateMemfd(byte[] so)
        {
            int fd = syscall_memfd(SYS_memfd_create_arm, "mlengine", MFD_CLOEXEC);
            int e = Marshal.GetLastWin32Error();
            Log.Trace("memfd fd=" + fd);
            if (fd < 0) throw new InvalidOperationException("memfd_create(syscall 385) failed: errno " + e);

            var pin = GCHandle.Alloc(so, GCHandleType.Pinned);
            try
            {
                IntPtr basePtr = pin.AddrOfPinnedObject();
                long total = 0;
                while (total < so.Length)
                {
                    IntPtr n = write(fd, basePtr + (int)total, (UIntPtr)(so.Length - total));
                    long w = n.ToInt64();
                    if (w <= 0) throw new IOException("write to memfd failed: " + Marshal.GetLastWin32Error());
                    total += w;
                }
            }
            finally { pin.Free(); }
            Log.Trace("wrote " + so.Length + " bytes to memfd " + fd);
            return fd;
        }

        // Load the engine image from an anonymous fd; on targets that restrict dlopen of a
        // mapped file, fall back to the manual ELF32-ARM loader (anonymous memory).
        public static IntPtr LoadFromMemory(byte[] so)
        {
            try
            {
                return Open("/proc/self/fd/" + CreateMemfd(so), "memfd");
            }
            catch (Exception memErr)
            {
                // Some targets restrict dlopen of a mapped file (a memfd included). Map the
                // image into plain anonymous memory ourselves. Keep the memfd reason if the
                // manual route fails too, so Engine.Load's packaged-file fallback still runs.
                Log.Error("memfd route failed: " + memErr.Message + "; trying the manual loader");
                try { s_image = ManualLoad(so); return s_image.Base; }
                catch (Exception manErr)
                { throw new InvalidOperationException(memErr.Message + "; manual loader: " + manErr.Message); }
            }
        }

        // The runtime binds a P/Invoke (and PtrToStringAnsi's helper) on first use with its
        // own dlsym, which overwrites the dlerror() state. Bind both before any dlopen whose
        // error we want to read, or the message is lost or garbled.
        private static bool s_warm;
        private static void Warm()
        {
            if (s_warm) return;
            IntPtr x = Marshal.StringToHGlobalAnsi("x");
            try { Marshal.PtrToStringAnsi(x); dlerror(); } finally { Marshal.FreeHGlobal(x); }
            s_warm = true;
        }

        // dlopen; null + reason (dlerror() text and errno) on failure.
        public static IntPtr TryOpen(string path, out string why)
        {
            Warm();
            dlerror();                                   // clear a stale error
            IntPtr h = dlopen(path, RTLD_NOW);
            int errno = Marshal.GetLastWin32Error();
            IntPtr err = h == IntPtr.Zero ? dlerror() : IntPtr.Zero;
            why = h != IntPtr.Zero ? null
                : (err != IntPtr.Zero ? Marshal.PtrToStringAnsi(err) : "no dlerror") + " (errno " + errno + ")";
            return h;
        }

        // The engine's own libraries (build-engine-so.sh link line): when the engine does
        // not load, name the first of them that does not load by itself.
        internal static readonly string[] EngineDeps = {
            "libesplusplayer.so", "libecore_wl2.so.1", "libecore.so.1",
            "libcrypto.so.1.0.0", "libssl.so.1.0.0", "libcurl.so.4",
        };

        public static IntPtr Open(string path, string what)
        {
            string why;
            IntPtr h = TryOpen(path, out why);
            if (h == IntPtr.Zero)
            {
                string deps = "";
                foreach (var d in EngineDeps)
                {
                    string dwhy;
                    if (TryOpen(d, out dwhy) == IntPtr.Zero) { deps = "; dependency " + d + ": " + dwhy; break; }
                }
                throw new InvalidOperationException("dlopen(" + what + ") failed: " + why + (deps == "" ? "; all dependencies load" : deps));
            }
            Log.Trace("dlopen(" + what + ") handle=0x" + h.ToInt64().ToString("x"));
            return h;
        }

        public static T GetDelegate<T>(IntPtr handle, string symbol) where T : class
        {
            IntPtr p = (s_image != null && handle == s_image.Base) ? s_image.Resolve(symbol) : dlsym(handle, symbol);
            if (p == IntPtr.Zero) throw new EntryPointNotFoundException("dlsym: " + symbol);
            Log.Trace("dlsym " + symbol + " -> 0x" + p.ToInt64().ToString("x"));
            return (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
        }

        // ---- Route 2: manual ELF32-ARM image loader ----
        // Fallback for targets that restrict dlopen of a mapped file: map the image into
        // plain anonymous memory, copy its PT_LOAD segments, apply its relocations, resolve
        // its imports from already-loaded dependencies, run its initializers, then resolve
        // its exports by name from its own symbol table. The engine .so is built to fit this
        // path: no TLS, SysV hash present (DT_HASH), and only the four ARM reloc types below.
        // There is no host-runnable test for this; it is verified on-device via `sdb dlog MLHOST`.
        private const int PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4;
        private const int MAP_PRIVATE = 2, MAP_ANONYMOUS = 0x20, Page = 4096;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate void InitFn();

        private static ManualImage s_image;   // kept for process life; its mmap is never unmapped

        private static long PageDown(long v) => v & ~((long)Page - 1);
        private static long PageUp(long v)   => (v + Page - 1) & ~((long)Page - 1);

        private static string CStr(byte[] b, int off)
        {
            int n = off;
            while (n < b.Length && b[n] != 0) n++;
            return Encoding.ASCII.GetString(b, off, n - off);
        }

        // A manually mapped image: resolves its exported symbols by name from its own symtab.
        private sealed class ManualImage
        {
            public readonly IntPtr Base;
            private readonly long _bias;
            private readonly byte[] _so;
            private readonly int _symOff, _symCount, _strOff;

            public ManualImage(IntPtr @base, long bias, byte[] so, int symOff, int symCount, int strOff)
            { Base = @base; _bias = bias; _so = so; _symOff = symOff; _symCount = symCount; _strOff = strOff; }

            public IntPtr Resolve(string name)
            {
                for (int i = 0; i < _symCount; i++)
                {
                    int e = _symOff + i * 16;                        // Elf32_Sym is 16 bytes
                    ushort shndx = BitConverter.ToUInt16(_so, e + 14);
                    if (shndx == 0) continue;                        // SHN_UNDEF: not defined here
                    uint stName = BitConverter.ToUInt32(_so, e);
                    if (CStr(_so, _strOff + (int)stName) != name) continue;
                    return (IntPtr)(_bias + BitConverter.ToUInt32(_so, e + 4));
                }
                return IntPtr.Zero;
            }
        }

        private static ManualImage ManualLoad(byte[] so)
        {
            if (so.Length < 52 || so[0] != 0x7F || so[1] != (byte)'E' || so[2] != (byte)'L' || so[3] != (byte)'F')
                throw new InvalidOperationException("not an ELF image");
            if (so[4] != 1) throw new InvalidOperationException("not ELFCLASS32");
            if (BitConverter.ToUInt16(so, 18) != 40) throw new InvalidOperationException("not ARM (e_machine != 40)");

            uint phoff = BitConverter.ToUInt32(so, 28);
            int phentsize = BitConverter.ToUInt16(so, 42);
            int phnum = BitConverter.ToUInt16(so, 44);

            // Program headers. loads entry = {offset, vaddr, filesz, memsz, flags}.
            var loads = new List<uint[]>();
            uint dynOff = 0, dynSize = 0;
            for (int i = 0; i < phnum; i++)
            {
                int p = (int)phoff + i * phentsize;
                uint type = BitConverter.ToUInt32(so, p);
                if (type == 1)                                       // PT_LOAD
                    loads.Add(new[] { BitConverter.ToUInt32(so, p + 4), BitConverter.ToUInt32(so, p + 8),
                                      BitConverter.ToUInt32(so, p + 16), BitConverter.ToUInt32(so, p + 20),
                                      BitConverter.ToUInt32(so, p + 24) });
                else if (type == 2)                                  // PT_DYNAMIC
                { dynOff = BitConverter.ToUInt32(so, p + 4); dynSize = BitConverter.ToUInt32(so, p + 20); }
            }
            if (loads.Count == 0) throw new InvalidOperationException("no PT_LOAD segments");
            if (dynOff == 0) throw new InvalidOperationException("no PT_DYNAMIC segment");

            long minV = long.MaxValue, maxV = 0;
            foreach (var l in loads)
            {
                long lo = PageDown(l[1]), hi = PageUp(l[1] + l[3]);
                if (lo < minV) minV = lo;
                if (hi > maxV) maxV = hi;
            }
            long span = maxV - minV;

            IntPtr @base = mmap(IntPtr.Zero, (UIntPtr)(ulong)span, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, IntPtr.Zero);
            if (@base == IntPtr.Zero || @base == (IntPtr)(-1))
                throw new InvalidOperationException("mmap failed: errno " + Marshal.GetLastWin32Error());
            long bias = @base.ToInt64() - minV;
            Log.Trace("manual loader: mapped span=0x" + span.ToString("x") + " at base=0x" + @base.ToInt64().ToString("x"));

            // Copy file-backed bytes of each PT_LOAD; the bss tail stays zero from the anon map.
            foreach (var l in loads)
                Marshal.Copy(so, (int)l[0], (IntPtr)(bias + l[1]), (int)l[2]);

            // A vaddr -> file offset, for tables that live in a file-backed segment.
            Func<uint, int> fileOff = (v) =>
            {
                foreach (var l in loads)
                    if (v >= l[1] && v < l[1] + l[2]) return (int)(l[0] + (v - l[1]));
                throw new InvalidOperationException("vaddr 0x" + v.ToString("x") + " not in a file-backed segment");
            };

            // Dynamic section (Elf32_Dyn is 8 bytes: d_tag i32, d_val u32).
            uint dtStrtab = 0, dtSymtab = 0, dtHash = 0, dtRel = 0, dtRelsz = 0, dtJmprel = 0, dtPltrelsz = 0;
            uint dtInit = 0, dtInitArray = 0, dtInitArraySz = 0;
            var needed = new List<uint>();
            int dcount = (int)(dynSize / 8);
            for (int i = 0; i < dcount; i++)
            {
                int d = (int)dynOff + i * 8;
                int tag = BitConverter.ToInt32(so, d);
                uint val = BitConverter.ToUInt32(so, d + 4);
                if (tag == 0) break;                                 // DT_NULL
                switch (tag)
                {
                    case 1:  needed.Add(val); break;                 // DT_NEEDED (string offset)
                    case 2:  dtPltrelsz = val; break;
                    case 4:  dtHash = val; break;
                    case 5:  dtStrtab = val; break;
                    case 6:  dtSymtab = val; break;
                    case 12: dtInit = val; break;
                    case 17: dtRel = val; break;
                    case 18: dtRelsz = val; break;
                    case 20: if (val != 17) throw new InvalidOperationException("DT_PLTREL is not REL"); break;
                    case 23: dtJmprel = val; break;
                    case 25: dtInitArray = val; break;
                    case 27: dtInitArraySz = val; break;
                }
            }
            if (dtStrtab == 0 || dtSymtab == 0 || dtHash == 0)
                throw new InvalidOperationException("missing DT_STRTAB/DT_SYMTAB/DT_HASH");

            int strOff = fileOff(dtStrtab);
            int symOff = fileOff(dtSymtab);
            int symCount = (int)BitConverter.ToUInt32(so, fileOff(dtHash) + 4);   // nchain == symbol count
            Log.Trace("manual loader: " + symCount + " dynamic symbols, " + needed.Count + " needed libs");

            // Load every NEEDED library and keep its handle. The .NET runtime may already
            // have some of them (libcrypto/libssl/curl) in its own RTLD_LOCAL scope: then
            // dlopen returns the existing handle WITHOUT promoting it to the global scope,
            // and dlsym(RTLD_DEFAULT) will not see its symbols. Resolving per-handle
            // (dlsym(handle, name)) works in both cases.
            var depHandles = new List<KeyValuePair<string, IntPtr>>();
            foreach (var nOff in needed)
            {
                string lib = CStr(so, strOff + (int)nOff);
                IntPtr h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
                Log.Trace("manual loader: dep " + lib + (h == IntPtr.Zero ? " FAILED: " + dlerror() : " ok handle=0x" + h.ToInt64().ToString("x")));
                if (h != IntPtr.Zero) depHandles.Add(new KeyValuePair<string, IntPtr>(lib, h));
            }

            // Resolve one symbol by index: defined here -> own address; else search the
            // dependency handles directly, then RTLD_DEFAULT as a last resort.
            Func<int, IntPtr> resolve = (symIndex) =>
            {
                int e = symOff + symIndex * 16;
                ushort shndx = BitConverter.ToUInt16(so, e + 14);
                if (shndx != 0) return (IntPtr)(bias + BitConverter.ToUInt32(so, e + 4));
                string name = CStr(so, strOff + (int)BitConverter.ToUInt32(so, e));
                IntPtr a = IntPtr.Zero;
                foreach (var d in depHandles)
                {
                    a = dlsym(d.Value, name);
                    if (a != IntPtr.Zero) break;
                }
                if (a == IntPtr.Zero) a = dlsym(IntPtr.Zero, name);
                if (a == IntPtr.Zero && (so[e + 12] >> 4) != 2)      // st_info>>4 == STB_WEAK(2) may stay 0
                    throw new InvalidOperationException("unresolved symbol: " + name +
                        " (searched " + depHandles.Count + " dependency handles and RTLD_DEFAULT)");
                return a;
            };

            // Relocations: DT_REL then DT_JMPREL. Both REL (Elf32_Rel is 8 bytes); the addend
            // is the word already in place. Semantics match glibc's ARM dynamic linker.
            int relative = 0, globDat = 0, jumpSlot = 0, abs32 = 0;
            Action<uint, uint> applyRel = (tblVaddr, tblSize) =>
            {
                if (tblVaddr == 0 || tblSize == 0) return;
                int off0 = fileOff(tblVaddr);
                int n = (int)(tblSize / 8);
                for (int i = 0; i < n; i++)
                {
                    int r = off0 + i * 8;
                    uint rOffset = BitConverter.ToUInt32(so, r);
                    uint rInfo = BitConverter.ToUInt32(so, r + 4);
                    int symIndex = (int)(rInfo >> 8);
                    int type = (int)(rInfo & 0xFF);
                    IntPtr P = (IntPtr)(bias + rOffset);
                    int A = Marshal.ReadInt32(P);                    // in-place addend
                    switch (type)
                    {
                        case 23: Marshal.WriteInt32(P, (int)(bias + (uint)A)); relative++; break;              // R_ARM_RELATIVE
                        case 21: Marshal.WriteInt32(P, (int)resolve(symIndex)); globDat++; break;              // R_ARM_GLOB_DAT
                        case 22: Marshal.WriteInt32(P, (int)resolve(symIndex)); jumpSlot++; break;             // R_ARM_JUMP_SLOT
                        case 2:  Marshal.WriteInt32(P, (int)((uint)A + (uint)resolve(symIndex))); abs32++; break; // R_ARM_ABS32
                        default: throw new NotSupportedException("unsupported reloc type " + type);
                    }
                }
            };
            applyRel(dtRel, dtRelsz);
            applyRel(dtJmprel, dtPltrelsz);
            Log.Trace("manual loader: relocs relative=" + relative + " glob_dat=" + globDat +
                      " jump_slot=" + jumpSlot + " abs32=" + abs32);

            // Segment permissions, after relocations (which wrote into the mapped pages).
            foreach (var l in loads)
            {
                int prot = (((l[4] & 1) != 0) ? PROT_EXEC : 0) |
                           (((l[4] & 2) != 0) ? PROT_WRITE : 0) |
                           (((l[4] & 4) != 0) ? PROT_READ : 0);
                long start = bias + PageDown(l[1]);
                long len = PageUp(l[1] + l[3]) - PageDown(l[1]);
                if (mprotect((IntPtr)start, (UIntPtr)(ulong)len, prot) != 0)
                    throw new InvalidOperationException("mprotect failed: errno " + Marshal.GetLastWin32Error());
            }

            // Initializers: DT_INIT then DT_INIT_ARRAY.
            if (dtInit != 0)
                ((InitFn)Marshal.GetDelegateForFunctionPointer((IntPtr)(bias + dtInit), typeof(InitFn)))();
            if (dtInitArray != 0)
            {
                int n = (int)(dtInitArraySz / 4);
                for (int i = 0; i < n; i++)
                {
                    int fn = Marshal.ReadInt32((IntPtr)(bias + dtInitArray + 4 * i));
                    if (fn == 0 || fn == -1) continue;
                    ((InitFn)Marshal.GetDelegateForFunctionPointer((IntPtr)fn, typeof(InitFn)))();
                }
                Log.Trace("manual loader: ran " + n + " init-array function(s)");
            }

            Log.Info("manual loader: image ready at base=0x" + @base.ToInt64().ToString("x"));
            return new ManualImage(@base, bias, so, symOff, symCount, strOff);
        }
    }

    // The engine C ABI (mlengine.h), bound at runtime from the in-memory .so.
    internal static class Engine
    {
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  StartDelegate(string host, int appId, string pin,
                                           int width, int height, int fps, int bitrateKbps, string datadir);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  RunningDelegate();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void WaitDelegate();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void StopDelegate();

        public static StartDelegate   Start;
        public static RunningDelegate Running;
        public static WaitDelegate    Wait;
        public static StopDelegate    Stop;

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int  MakeCertDelegate(out IntPtr certPem, out IntPtr keyPem);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  HttpInitDelegate(string certPem, string keyPem, string uniqueId);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  OpenUrlDelegate(string url, string ppk, out IntPtr data, out int len);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  PairDelegate(int serverMajor, string address, int httpPort, string pin, out IntPtr serverCertPem);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void CancelRequestDelegate();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  StartSessionDelegate(string host, int width, int height, int fps, int bitrateKbps,
                                                  string rikeyHex, int rikeyId, string appVersion,
                                                  string gfeVersion, string rtspUrl,
                                                  int serverCodecModeSupport, string videoCodec, int hdr,
                                                  string audioConfig);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SendKeyDelegate(int keyCode, int keyAction, int modifiers);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void FreeDelegate(IntPtr p);

        public static MakeCertDelegate      MakeCert;
        public static HttpInitDelegate      HttpInit;
        public static OpenUrlDelegate       OpenUrl;
        public static PairDelegate          Pair;
        public static CancelRequestDelegate CancelRequest;
        public static StartSessionDelegate  StartSession;
        public static SendKeyDelegate       SendKey;
        public static FreeDelegate          Free;
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void LogCbDelegate(IntPtr line);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetLogCbDelegate(LogCbDelegate cb);
        public static SetLogCbDelegate      SetLogCb;   // null with an engine that lacks it
        public static volatile bool         Loaded;
        public static string                LoadError = "not tried";   // reason shown in the UI's error replies

        public static void Load(string soPath)
        {
            byte[] bytes = File.ReadAllBytes(soPath);
            Log.Info("engine load: " + soPath + " (" + bytes.Length + " bytes), trying memfd");
            IntPtr h;
            try { h = MemLoader.LoadFromMemory(bytes); Log.Info("engine load: branch=memfd ok"); }
            catch (Exception memErr)
            {
                // The standard way: dlopen the packaged file itself. Report both reasons
                // if this fails too.
                Log.Error("engine load: branch=memfd failed: " + memErr.Message + "; trying the packaged file");
                try { h = MemLoader.Open(soPath, "file"); Log.Info("engine load: branch=file ok"); }
                catch (Exception fileErr)
                {
                    Log.Error("engine load: branch=file failed: " + fileErr.Message);
                    throw new InvalidOperationException(memErr.Message + "; " + fileErr.Message);
                }
            }
            Start   = MemLoader.GetDelegate<StartDelegate>(h,   "ml_engine_start");
            Running = MemLoader.GetDelegate<RunningDelegate>(h, "ml_engine_running");
            Wait    = MemLoader.GetDelegate<WaitDelegate>(h,    "ml_engine_wait");
            Stop    = MemLoader.GetDelegate<StopDelegate>(h,    "ml_engine_stop");
            MakeCert      = MemLoader.GetDelegate<MakeCertDelegate>(h,      "ml_engine_make_cert");
            HttpInit      = MemLoader.GetDelegate<HttpInitDelegate>(h,      "ml_engine_http_init");
            OpenUrl       = MemLoader.GetDelegate<OpenUrlDelegate>(h,       "ml_engine_open_url");
            Pair          = MemLoader.GetDelegate<PairDelegate>(h,          "ml_engine_pair");
            CancelRequest = MemLoader.GetDelegate<CancelRequestDelegate>(h, "ml_engine_cancel_request");
            StartSession  = MemLoader.GetDelegate<StartSessionDelegate>(h,  "ml_engine_start_session");
            SendKey       = MemLoader.GetDelegate<SendKeyDelegate>(h,       "ml_engine_send_key");
            Free          = MemLoader.GetDelegate<FreeDelegate>(h,          "ml_engine_free");
            try { SetLogCb = MemLoader.GetDelegate<SetLogCbDelegate>(h, "ml_engine_set_log_cb"); }
            catch (EntryPointNotFoundException) { SetLogCb = null; }
            Loaded = true;
        }
    }

    // UI command port "moonlight" (pkg/ui/PROTOCOL.md): runs commands, sends replies and events.
    internal static class CommandPort
    {
        private const string DefaultApp  = "Moonlight5.ui";
        private const string DefaultPort = "moonlightui";

        private static Tizen.Applications.Messages.MessagePort s_port;  // kept for process life
        private static System.Threading.Timer s_monitor;
        private static readonly object s_httpLock = new object();  // libgamestream has one global cancel flag
        private static volatile bool s_active;
        private static string s_uiApp = DefaultApp;
        private static string s_uiPort = DefaultPort;

        public static void Start()
        {
            s_port = new Tizen.Applications.Messages.MessagePort("moonlight", false);
            s_port.MessageReceived += OnMessage;
            s_port.Listen();
            s_monitor = new System.Threading.Timer(_ => Poll(), null, 500, 500);
            Log.Trace("command port 'moonlight' listening");
        }

        private static void Poll()
        {
            try
            {
                if (s_active && Engine.Loaded && Engine.Running() == 0)
                {
                    s_active = false;
                    SendEvent("streamTerminated: 0");
                }
            }
            catch (Exception ex) { Log.Error("monitor failed: " + ex.Message); }
        }

        private static string Item(Tizen.Applications.Bundle b, string k)
            => b.Contains(k) ? b.GetItem<string>(k) : null;

        private static void OnMessage(object sender, Tizen.Applications.Messages.MessageReceivedEventArgs e)
        {
            try
            {
                var b = e.Message;
                string app = e.Remote?.AppId, port = e.Remote?.PortName;
                if (!string.IsNullOrEmpty(app)) s_uiApp = app;
                if (!string.IsNullOrEmpty(port)) s_uiPort = port;
                string id = Item(b, "id");
                string cmd = Item(b, "cmd");
                if (id == null) { Log.Error("command without id ignored: " + cmd); return; }
                if (Item(b, "total") != null) { b = Join(id, b); if (b == null) return; }   // not all parts yet
                Log.Trace("cmd " + cmd + " id=" + id);
                string replyApp = s_uiApp, replyPort = s_uiPort;
                Task.Run(() => Run(b, cmd ?? "", id, replyApp, replyPort));
            }
            catch (Exception ex) { Log.Error("command receive failed: " + ex.Message); }
        }

        private static void Run(Tizen.Applications.Bundle b, string cmd, string id, string app, string port)
        {
            var sw = System.Diagnostics.Stopwatch.StartNew();
            var reply = new Dictionary<string, object>();
            string err;
            try { err = Execute(b, cmd, reply); }
            catch (Exception ex) { Log.Error("cmd " + cmd + " failed: " + ex); err = "exception"; }
            if (err != null) { reply.Clear(); reply["error"] = err; }   // drop partial results
            SendReply(id, reply, app, port);
            Log.Trace("reply " + id + (err == null ? " ok " : " err ") + sw.ElapsedMilliseconds + "ms");

            if (cmd == "startStream")
            {
                if (err == null) { s_active = true; SendEvent("Connection Established"); }
                else SendEvent("streamTerminated: -1");
            }
        }

        private static bool Send(Tizen.Applications.Bundle msg, string app, string port)
        {
            try { s_port.Send(msg, app, port); return true; }
            catch (Exception ex) { Log.Error("send failed: " + ex.Message); return false; }
        }

        // A message port message is limited to ~4 KB, so a large message travels as
        // ordered parts {id, seq, total, field, chunk} (pkg/ui/PROTOCOL.md). The sizes
        // leave room for the Bundle encoding overhead.
        private const int MaxSingle = 1536, ChunkBytes = 1536, ChunkChars = 512;   // 512 chars <= 1536 UTF-8 bytes

        private static void SendReply(string id, Dictionary<string, object> r, string app, string port)
        {
            int size = 0;
            foreach (var kv in r)
                size += kv.Key.Length + (kv.Value is byte[] ? ((byte[])kv.Value).Length : Encoding.UTF8.GetByteCount((string)kv.Value));
            if (size <= MaxSingle)
            {
                var m = new Tizen.Applications.Bundle();
                m.AddItem("id", id);
                foreach (var kv in r)
                    if (kv.Value is byte[]) m.AddItem(kv.Key, (byte[])kv.Value); else m.AddItem(kv.Key, (string)kv.Value);
                Send(m, app, port);
                return;
            }
            var parts = new List<KeyValuePair<string, object>>();
            foreach (var kv in r)
            {
                var bytes = kv.Value as byte[];
                int off = 0;
                if (bytes != null)
                    do
                    {
                        int n = Math.Min(ChunkBytes, bytes.Length - off);
                        var c = new byte[n];
                        Array.Copy(bytes, off, c, 0, n);
                        parts.Add(new KeyValuePair<string, object>(kv.Key, c));
                        off += n;
                    } while (off < bytes.Length);
                else
                {
                    string str = (string)kv.Value;
                    do
                    {
                        int n = Math.Min(ChunkChars, str.Length - off);
                        if (n > 1 && off + n < str.Length && char.IsHighSurrogate(str[off + n - 1])) n--;   // keep pairs whole
                        parts.Add(new KeyValuePair<string, object>(kv.Key, str.Substring(off, n)));
                        off += n;
                    } while (off < str.Length);
                }
            }
            for (int seq = 0; seq < parts.Count; seq++)
            {
                var m = new Tizen.Applications.Bundle();
                m.AddItem("id", id);
                m.AddItem("seq", seq.ToString());
                m.AddItem("total", parts.Count.ToString());
                m.AddItem("field", parts[seq].Key);
                if (parts[seq].Value is byte[]) m.AddItem("chunk", (byte[])parts[seq].Value);
                else m.AddItem("chunk", (string)parts[seq].Value);
                if (!Send(m, app, port)) return;   // the UI times out the command
            }
            Log.Trace("reply " + id + " sent in " + parts.Count + " parts");
        }

        private sealed class Partial
        {
            public int Total;
            public readonly Dictionary<int, KeyValuePair<string, string>> Seqs = new Dictionary<int, KeyValuePair<string, string>>();
        }
        private static readonly Dictionary<string, Partial> s_partial = new Dictionary<string, Partial>();

        // Collects the parts of a chunked command; returns the joined command once all arrived.
        private static Tizen.Applications.Bundle Join(string id, Tizen.Applications.Bundle b)
        {
            int seq, total;
            if (!int.TryParse(Item(b, "seq"), out seq) || !int.TryParse(Item(b, "total"), out total) ||
                seq < 0 || seq >= total || Item(b, "field") == null)
            {
                Log.Error("bad command part for id=" + id);
                return null;
            }
            Partial a;
            lock (s_partial)
            {
                if (!s_partial.TryGetValue(id, out a)) { a = new Partial { Total = total }; s_partial[id] = a; }
                if (!a.Seqs.ContainsKey(seq)) a.Seqs[seq] = new KeyValuePair<string, string>(Item(b, "field"), Item(b, "chunk") ?? "");
                if (a.Seqs.Count < a.Total) return null;
                s_partial.Remove(id);
            }
            var fieldsJoined = new Dictionary<string, StringBuilder>();
            for (int i = 0; i < a.Total; i++)
            {
                KeyValuePair<string, string> pt;
                if (!a.Seqs.TryGetValue(i, out pt)) { Log.Error("command part missing for id=" + id); return null; }
                StringBuilder sb;
                if (!fieldsJoined.TryGetValue(pt.Key, out sb)) { sb = new StringBuilder(); fieldsJoined[pt.Key] = sb; }
                sb.Append(pt.Value);
            }
            var joined = new Tizen.Applications.Bundle();
            joined.AddItem("cmd", Item(b, "cmd") ?? "");
            joined.AddItem("id", id);
            foreach (var kv in fieldsJoined) joined.AddItem(kv.Key, kv.Value.ToString());
            Log.Trace("joined id=" + id + " parts=" + a.Total);
            return joined;
        }

        // Log forward: errors go straight to dlog (never back through Log, no loop).
        public static void SendLog(string tag, string line)
        {
            if (s_port == null) return;
            try
            {
                var m = new Tizen.Applications.Bundle();
                m.AddItem("log", line);
                m.AddItem("tag", tag);
                s_port.Send(m, s_uiApp, s_uiPort);
            }
            catch (Exception ex) { Tizen.Log.Error("MLHOST", "log forward send failed: " + ex.Message); }
        }

        private static void SendEvent(string ev)
        {
            var m = new Tizen.Applications.Bundle();
            m.AddItem("event", ev);
            Send(m, s_uiApp, s_uiPort);
        }

        private static string Take(IntPtr p)
        {
            if (p == IntPtr.Zero) return null;
            string s = Marshal.PtrToStringAnsi(p);
            Engine.Free(p);
            return s;
        }

        private static string Rc(int rc) => rc == 0 ? null : rc.ToString();

        // Returns null on success (result fields in r) or the error string. Never logs secrets.
        private static string Execute(Tizen.Applications.Bundle b, string cmd, Dictionary<string, object> r)
        {
            if (cmd == "setLog") { UiLog.Enable(Item(b, "on") == "true"); return null; }   // works without the engine too
            if (cmd == "diag")                                                              // also without the engine
            {
                if (Item(b, "rerun") == "true") Diag.Run("run on request from the UI");
                r.Add("report", Diag.Report());
                return null;
            }
            if (!Engine.Loaded) return "engine not loaded: " + Engine.LoadError;
            switch (cmd)
            {
                case "makeCert":
                    lock (s_httpLock)
                    {
                        IntPtr c, k;
                        int rc = Engine.MakeCert(out c, out k);
                        string cert = Take(c), key = Take(k);
                        if (rc != 0) return rc.ToString();
                        r.Add("cert", cert ?? ""); r.Add("key", key ?? "");
                        return null;
                    }
                case "httpInit":
                    lock (s_httpLock)
                        return Rc(Engine.HttpInit(Item(b, "cert"), Item(b, "key"), Item(b, "uniqueId")));
                case "openUrl":
                    lock (s_httpLock)
                    {
                        IntPtr p; int len;
                        int rc = Engine.OpenUrl(Item(b, "url"), Item(b, "ppk"), out p, out len);
                        byte[] data = new byte[0];
                        if (p != IntPtr.Zero)
                        {
                            if (len > 0) { data = new byte[len]; Marshal.Copy(p, data, 0, len); }
                            Engine.Free(p);
                        }
                        if (rc != 0) return rc.ToString();
                        if (Item(b, "binary") == "true") r.Add("data", data);
                        else r.Add("data", Encoding.UTF8.GetString(data));
                        return null;
                    }
                case "pair":
                    {
                        int major, port;
                        if (!int.TryParse(Item(b, "major"), out major) || !int.TryParse(Item(b, "port"), out port)) return "bad args";
                        lock (s_httpLock)
                        {
                            IntPtr c;
                            int rc = Engine.Pair(major, Item(b, "addr"), port, Item(b, "pin"), out c);
                            string cert = Take(c);
                            if (rc != 0) return rc.ToString();
                            r.Add("serverCert", cert ?? "");
                            return null;
                        }
                    }
                case "cancelRequest": Engine.CancelRequest(); return null;
                case "startStream":
                    {
                        int w, h, fps, br, rikeyid, codec;
                        if (!int.TryParse(Item(b, "w"), out w) || !int.TryParse(Item(b, "h"), out h) ||
                            !int.TryParse(Item(b, "fps"), out fps) || !int.TryParse(Item(b, "bitrate"), out br) ||
                            !int.TryParse(Item(b, "rikeyid"), out rikeyid))
                            return "bad args";
                        if (!int.TryParse(Item(b, "codecMode"), out codec)) codec = 0;   // optional: engine defaults to H.264
                        return Rc(Engine.StartSession(Item(b, "host"), w, h, fps, br, Item(b, "rikey"), rikeyid,
                                                      Item(b, "appVer"), Item(b, "gfeVer"), Item(b, "rtspUrl"),
                                                      codec, Item(b, "videoCodec"), Item(b, "hdr") == "true" ? 1 : 0,
                                                      Item(b, "audioConfig")));
                    }
                case "stopStream": Engine.Stop(); return null;
                case "sendKeyboardEvent":
                    {
                        int key, action, mods;
                        if (!int.TryParse(Item(b, "key"), out key) || !int.TryParse(Item(b, "action"), out action) ||
                            !int.TryParse(Item(b, "mods"), out mods)) return "bad args";
                        Engine.SendKey(key, action, mods);
                        return null;
                    }
                case "toggleStats": return null;
                case "stun": r.Add("result", ""); return null;
                case "wakeOnLan": return "not supported";
                default: return "unknown cmd " + cmd;
            }
        }
    }

    internal sealed class App : CoreUIApplication
    {
#if DEBUG
        // Relays the web UI console (mltrace.js) to dlog tag MLUI. Kept for process life.
        private static Tizen.Applications.Messages.MessagePort s_uiLog;

        private static void StartUiLog()
        {
            s_uiLog = new Tizen.Applications.Messages.MessagePort("mllog", false);
            s_uiLog.MessageReceived += (sender, e) =>
            {
                try
                {
                    var b = e.Message;
                    string level = b.Contains("level") ? b.GetItem<string>("level") : null;
                    string msg = b.Contains("msg") ? b.GetItem<string>("msg") : "";
                    if (level == "error") Tizen.Log.Error("MLUI", msg);
                    else if (level == "warn") Tizen.Log.Warn("MLUI", msg);
                    else Tizen.Log.Info("MLUI", msg);
                }
                catch (Exception ex) { Tizen.Log.Error("MLHOST", "ui log message failed: " + ex.Message); }
            };
            s_uiLog.Listen();
            Log.Trace("ui log port 'mllog' listening");
        }
#endif

        protected override void OnCreate()
        {
            base.OnCreate();
#if DEBUG
            try { StartUiLog(); }
            catch (Exception e) { Log.Error("ui log listener failed: " + e); }
#endif
            Log.Trace("OnCreate start");
            try
            {
                // The SDK packs the csproj Link "res/lib/..." next to the assembly (bin/res/lib/)
                // and also into lib/. Take the
                // first that exists and name every path tried when none does.
                string bin = Path.GetDirectoryName(typeof(App).Assembly.Location);
                string[] tried = {
                    Path.Combine(bin, "res", "lib", "libmlengine.so"),
                    Path.Combine(bin, "..", "lib", "libmlengine.so"),
                    Path.Combine(DirectoryInfo.Resource, "lib", "libmlengine.so"),
                };
                Diag.Setup(DirectoryInfo.Data, DirectoryInfo.Resource, tried);
#if DEBUG
                try { Diag.Run("start-up, before the engine load"); }
                catch (Exception e) { Log.Error("diagnostics failed: " + e); }   // never block the real load
#endif
                foreach (var t in tried) Log.Trace("engine candidate " + t + " exists=" + File.Exists(t));
                string soPath = Array.Find(tried, File.Exists);
                if (soPath == null) throw new FileNotFoundException("libmlengine.so not found in: " + string.Join(", ", tried));
                Engine.Load(soPath);
                // No session starts here: the web UI drives the engine over the message port.
                Log.Info("engine loaded: " + soPath);
            }
            catch (Exception e)
            {
                Engine.LoadError = e.GetType().Name + ": " + e.Message;
                Log.Error("engine load failed: " + e);
            }
            // Also after a failed load: every command then answers "engine not loaded".
            try { CommandPort.Start(); }
            catch (Exception e) { Log.Error("command port failed: " + e); }
        }

        protected override void OnTerminate()
        {
            Log.Trace("OnTerminate");
            try { Log.Trace("Engine.Stop"); Engine.Stop?.Invoke(); } catch { }
            base.OnTerminate();
        }

        private static void Main(string[] args) => new App().Run(args);
    }
}

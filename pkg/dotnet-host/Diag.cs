/*
 * Engine start-up diagnostics.
 *
 * Logs how this process was started (the launch arguments), the process context
 * (ids, capabilities, security label, cgroup, parent), where the engine file lies
 * (path, size, sha256, mount, extended attributes), and then tries every way of
 * loading libmlengine.so on its own, closing each again:
 *
 *   file       mmap probe (read, read+exec) + dlopen of the packaged file
 *   realpath   the same through the resolved path
 *   memfd      the bytes in an anonymous fd: mmap probe + dlopen /proc/self/fd/N
 *   data-copy  a copy in the app's data folder (diagnostic only, never used to run)
 *   deps       dlopen of each library the engine links against
 *
 * Every line names the step and the branch taken, with errno and dlerror text.
 * Debug builds run it at start-up, before the real load; the "diag" command
 * (pkg/ui/PROTOCOL.md, mlDiag() in the UI console) runs it again in any build.
 * Lines go to the host log (dlog MLHOST + the UI console) and <data>/mlhost-diag.log.
 */
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace MoonlightHost
{
    internal static class Diag
    {
        private const int O_RDONLY = 0, O_CLOEXEC = 0x80000;   // ARM values
        private const int PROT_READ = 1, PROT_WRITE = 2, PROT_EXEC = 4, MAP_PRIVATE = 2, PAGE = 4096;
        private const int MAP_ANONYMOUS = 0x20;                 // ARM Linux
        private static readonly IntPtr MAP_FAILED = new IntPtr(-1);
        private static readonly string[] Xattrs = {
            "security.SMACK64", "security.SMACK64EXEC", "security.SMACK64MMAP", "security.SMACK64TRANSMUTE",
        };

        [DllImport("libc", SetLastError = true)] private static extern int open(string path, int flags);
        [DllImport("libc")]                      private static extern int close(int fd);
        [DllImport("libc", SetLastError = true)] private static extern IntPtr mmap(IntPtr addr, UIntPtr len, int prot, int flags, int fd, int off);
        [DllImport("libc")]                      private static extern int munmap(IntPtr addr, UIntPtr len);
        [DllImport("libc", SetLastError = true)] private static extern int mprotect(IntPtr addr, UIntPtr len, int prot);
        [DllImport("libc", SetLastError = true)] private static extern IntPtr lgetxattr(string path, string name, byte[] value, UIntPtr size);
        [DllImport("libc", SetLastError = true)] private static extern IntPtr realpath(string path, IntPtr resolved);
        [DllImport("libc")]                      private static extern void free(IntPtr p);
        [DllImport("libc")]                      private static extern IntPtr strerror(int errnum);
        [DllImport("libdl.so.2")]                private static extern int dlclose(IntPtr handle);

        private static readonly object s_run = new object();
        private static readonly List<string> s_lines = new List<string>();
        private static string s_dataDir, s_resDir, s_file;
        private static string[] s_candidates = new string[0];
        private static int s_runs;

#if DEBUG
        private const string Variant = "debug";
#else
        private const string Variant = "release";
#endif

        public static void Setup(string dataDir, string resDir, string[] engineCandidates)
        {
            s_dataDir = dataDir; s_resDir = resDir; s_candidates = engineCandidates ?? new string[0];
        }

        // The lines of the last run.
        public static string Report()
        {
            lock (s_lines) return s_lines.Count == 0 ? "no diagnostics run yet" : string.Join("\n", s_lines);
        }

        public static void Run(string why)
        {
            lock (s_run)
            {
                s_runs++;
                lock (s_lines) s_lines.Clear();
                s_file = null;
                if (s_dataDir != null)
                {
                    string f = Path.Combine(s_dataDir, "mlhost-diag.log");
                    try { File.WriteAllText(f, ""); s_file = f; }
                    catch (Exception e) { W("file", "no log file " + f + ": " + e.Message); }
                }
                var sum = new List<string>();
                W("run", why + ", " + Variant + " build, host " + typeof(Diag).Assembly.GetName().Version +
                         ", " + DateTime.UtcNow.ToString("yyyy-MM-dd HH:mm:ss") + " UTC");
                Step("process", Process);
                Step("launch", Launch);
                Step("paths", Paths);
                string so = Array.Find(s_candidates, File.Exists);
                foreach (var c in s_candidates) W("engine", "candidate " + c + " exists=" + File.Exists(c));
                if (so == null) { W("engine", "branch=no-file: no engine file found, load routes skipped"); sum.Add("engine file missing"); }
                else
                {
                    Step("file", () => sum.Add(FileRoute("file", so)));
                    Step("realpath", () =>
                    {
                        string rp = Real(so);
                        if (rp == null || rp == so) { W("realpath", "branch=same-path: " + (rp ?? "unresolved") + ", skipped"); sum.Add("realpath: same as file"); }
                        else sum.Add(FileRoute("realpath", rp));
                    });
                    Step("memfd", () => sum.Add(MemfdRoute(so)));
                    Step("data-copy", () => sum.Add(DataCopyRoute(so)));
                }
                Step("anon-exec", () => sum.Add(AnonExecRoute()));
                Step("deps", () => sum.Add(Deps()));
                W("summary", string.Join("; ", sum));
            }
        }

        private static void Step(string name, Action a)
        {
            try { a(); }
            catch (Exception e) { W(name, "step failed: " + e.GetType().Name + ": " + e.Message); }
        }

        private static void W(string step, string text)
        {
            string line = "diag#" + s_runs + " " + step + ": " + text;
            lock (s_lines) s_lines.Add(line);
            Log.Info(line);
            string f = s_file;
            if (f == null) return;
            try { File.AppendAllText(f, DateTime.UtcNow.ToString("HH:mm:ss.fff") + " " + line + "\n"); }
            catch (Exception e) { s_file = null; W("file", "log file off: " + e.Message); }
        }

        private static string Errno(int e)
        {
            string s = null;
            try { s = Marshal.PtrToStringAnsi(strerror(e)); } catch { }
            return "errno " + e + (s != null ? " (" + s + ")" : "");
        }

        private static string Read(string path)
        {
            try { return File.ReadAllText(path).Replace('\0', ' ').Trim(); }
            catch (Exception e) { return "unreadable (" + e.GetType().Name + ": " + e.Message + ")"; }
        }

        private static string Real(string path)
        {
            IntPtr p = realpath(path, IntPtr.Zero);
            if (p == IntPtr.Zero) return null;
            try { return Marshal.PtrToStringAnsi(p); } finally { free(p); }
        }

        // --- process context ---

        private static readonly string[] StatusKeys = {
            "Name", "Pid", "PPid", "Uid", "Gid", "Groups", "CapInh", "CapPrm", "CapEff", "CapBnd", "CapAmb", "NoNewPrivs", "Seccomp",
        };

        private static void Process()
        {
            string ppid = null;
            foreach (var l in File.ReadAllLines("/proc/self/status"))
            {
                int c = l.IndexOf(':');
                if (c <= 0 || Array.IndexOf(StatusKeys, l.Substring(0, c)) < 0) continue;
                string v = l.Substring(c + 1).Trim();
                if (l.StartsWith("PPid:")) ppid = v;
                W("process", l.Substring(0, c) + " = " + v);
            }
            W("process", "label = " + Read("/proc/self/attr/current"));
            W("process", "cgroup = " + Read("/proc/self/cgroup").Replace('\n', '|'));
            W("process", "runtime " + Environment.Version + ", " + IntPtr.Size * 8 + "-bit, " + Environment.OSVersion);
            if (ppid != null)
                W("parent", "pid " + ppid + " cmdline = " + Read("/proc/" + ppid + "/cmdline") +
                            ", label = " + Read("/proc/" + ppid + "/attr/current"));
        }

        // Launch arguments: each value is a bundle record in base64; decode the key/value.
        private static void Launch()
        {
            string[] argv = File.ReadAllText("/proc/self/cmdline").TrimEnd('\0').Split('\0');
            W("launch", argv.Length + " arguments");
            for (int i = 0; i < argv.Length; i++)
            {
                string kv = BundleRecord(argv[i]);
                if (kv != null) W("launch", "  " + kv);
                else if (i == 0 || !argv[i].StartsWith("__") || i + 1 >= argv.Length || BundleRecord(argv[i + 1]) == null)
                    W("launch", "  argv[" + i + "] = " + (argv[i].Length > 200 ? argv[i].Substring(0, 200) + "..." : argv[i]));
            }
        }

        // [total][type][keylen][key\0][vallen][value\0], little endian; null if not one.
        internal static string BundleRecord(string s)
        {
            if (s.Length < 16 || s.Length % 4 != 0) return null;
            byte[] b;
            try { b = Convert.FromBase64String(s); } catch (FormatException) { return null; }
            if (b.Length < 16) return null;
            int klen = BitConverter.ToInt32(b, 8);
            if (klen < 1 || 12 + klen + 4 > b.Length) return null;
            string key = Encoding.ASCII.GetString(b, 12, klen - 1);
            int vo = 12 + klen, vlen = BitConverter.ToInt32(b, vo);
            if (vlen < 0 || vo + 4 + vlen > b.Length) return key + " = (unparsed value)";
            string val = Encoding.UTF8.GetString(b, vo + 4, Math.Max(0, vlen - 1));
            return key + " = " + val;
        }

        // --- files and mounts ---

        private static void Paths()
        {
            string asm = typeof(Diag).Assembly.Location;
            W("paths", "assembly " + asm + " -> " + (Real(asm) ?? "unresolved"));
            W("paths", "resource dir " + s_resDir + " -> " + (s_resDir == null ? "-" : Real(s_resDir) ?? "unresolved"));
            W("paths", "data dir " + s_dataDir + " -> " + (s_dataDir == null ? "-" : Real(s_dataDir) ?? "unresolved"));
            if (s_dataDir != null) W("paths", "data dir mount: " + Mount(Real(s_dataDir) ?? s_dataDir));
            try { W("paths", "assembly sha256 " + Sha(asm)); } catch (Exception e) { W("paths", "assembly sha256 failed: " + e.Message); }
        }

        // The mount that holds path (longest mount point that prefixes it).
        private static string Mount(string path)
        {
            string best = null; int bestLen = -1;
            foreach (var l in File.ReadAllLines("/proc/self/mountinfo"))
            {
                // id parent maj:min root mountpoint options [optional...] - fstype source superoptions
                string[] f = l.Split(' ');
                if (f.Length < 10) continue;
                string mp = f[4].Replace("\\040", " ");
                bool under = path == mp || mp == "/" || path.StartsWith(mp + "/");
                if (!under || mp.Length <= bestLen) continue;
                int dash = Array.IndexOf(f, "-");
                best = mp + " opts=" + f[5] + (dash > 0 && dash + 3 < f.Length ? " type=" + f[dash + 1] + " src=" + f[dash + 2] + " super=" + f[dash + 3] : "");
                bestLen = mp.Length;
            }
            return best ?? "not found";
        }

        private static string Sha(string path)
        {
            using (var s = File.OpenRead(path))
            using (var h = SHA256.Create())
                return BitConverter.ToString(h.ComputeHash(s)).Replace("-", "").ToLowerInvariant();
        }

        private static string XattrList(string path)
        {
            var sb = new StringBuilder();
            foreach (var name in Xattrs)
            {
                var buf = new byte[256];
                long n = lgetxattr(path, name, buf, (UIntPtr)buf.Length).ToInt64();
                int e = Marshal.GetLastWin32Error();
                sb.Append(name.Substring(9)).Append('=');
                if (n >= 0) sb.Append('"').Append(Encoding.ASCII.GetString(buf, 0, (int)Math.Min(n, buf.Length)).TrimEnd('\0')).Append('"');
                else sb.Append(e == 61 ? "none" : Errno(e));   // 61 = ENODATA
                sb.Append(' ');
            }
            return sb.ToString().TrimEnd();
        }

        private static void Describe(string step, string path)
        {
            var fi = new FileInfo(path);
            W(step, path + " size=" + fi.Length + " mtime=" + fi.LastWriteTimeUtc.ToString("yyyy-MM-dd HH:mm:ss") + "Z");
            Step(step, () => W(step, "realpath " + (Real(path) ?? "unresolved")));
            Step(step, () => W(step, "sha256 " + Sha(path)));
            Step(step, () => W(step, "mount " + Mount(Real(path) ?? path)));
            Step(step, () => W(step, "xattr " + XattrList(path)));
        }

        // --- load routes ---

        // mmap the first page of fd read-only, then read+exec. "read=ok exec=errno 13 (...)".
        private static string MmapFd(string step, int fd)
        {
            var r = new StringBuilder();
            foreach (int prot in new[] { PROT_READ, PROT_READ | PROT_EXEC })
            {
                string what = prot == PROT_READ ? "read" : "exec";
                IntPtr p = mmap(IntPtr.Zero, (UIntPtr)PAGE, prot, MAP_PRIVATE, fd, 0);
                int e = Marshal.GetLastWin32Error();
                if (p == MAP_FAILED) { W(step, "mmap(" + what + ") branch=failed " + Errno(e)); r.Append(what).Append("=failed(").Append(e).Append(") "); }
                else { W(step, "mmap(" + what + ") branch=ok"); munmap(p, (UIntPtr)PAGE); r.Append(what).Append("=ok "); }
            }
            return r.ToString().TrimEnd();
        }

        private static string MmapPath(string step, string path)
        {
            int fd = open(path, O_RDONLY | O_CLOEXEC);
            int e = Marshal.GetLastWin32Error();
            if (fd < 0) { W(step, "open branch=failed " + Errno(e)); return "open failed(" + e + ")"; }
            try { return MmapFd(step, fd); } finally { close(fd); }
        }

        private static string Dlopen(string step, string path)
        {
            var sw = Stopwatch.StartNew();
            string why;
            IntPtr h = MemLoader.TryOpen(path, out why);
            if (h == IntPtr.Zero) { W(step, "dlopen(" + path + ") branch=failed in " + sw.ElapsedMilliseconds + " ms: " + why); Maps(step); return "dlopen failed"; }
            IntPtr sym = MemLoader.dlsym(h, "ml_engine_start");
            W(step, "dlopen(" + path + ") branch=ok handle=0x" + h.ToInt64().ToString("x") + " in " + sw.ElapsedMilliseconds +
                    " ms, ml_engine_start " + (sym == IntPtr.Zero ? "MISSING" : "0x" + sym.ToInt64().ToString("x")));
            Maps(step);
            W(step, "dlclose -> " + dlclose(h));
            return "dlopen ok";
        }

        private static void Maps(string step)
        {
            int n = 0;
            foreach (var l in File.ReadAllLines("/proc/self/maps"))
                if ((l.Contains("mlengine") || l.Contains("memfd:")) && n++ < 12) W(step, "maps " + l);
            if (n == 0) W(step, "maps: no engine mapping");
        }

        private static string FileRoute(string step, string path)
        {
            Describe(step, path);
            return step + ": " + MmapPath(step, path) + ", " + Dlopen(step, path);
        }

        private static string MemfdRoute(string so)
        {
            int fd = MemLoader.CreateMemfd(File.ReadAllBytes(so));
            W("memfd", "fd " + fd + " -> " + (Real("/proc/self/fd/" + fd) ?? "unresolved"));
            try { return "memfd: " + MmapFd("memfd", fd) + ", " + Dlopen("memfd", "/proc/self/fd/" + fd); }
            finally { close(fd); }
        }

        // Diagnostic only: tells "this file or its folder" from "this process" when the
        // packaged file does not load. The copy is deleted again and never used to run.
        private static string DataCopyRoute(string so)
        {
            if (s_dataDir == null) { W("data-copy", "branch=no-data-dir, skipped"); return "data-copy: skipped"; }
            string dst = Path.Combine(s_dataDir, "diag-libmlengine.so");
            File.Copy(so, dst, true);
            try { return FileRoute("data-copy", dst); }
            finally { try { File.Delete(dst); } catch (Exception e) { W("data-copy", "delete failed: " + e.Message); } }
        }

        // The loader's (and the JIT's) mechanism, probed directly: anonymous RW memory,
        // mprotect to RX, then call a two-instruction ARM function through a delegate.
        // fd-backed exec-mmap may be refused on this target while this still works.
        private static string AnonExecRoute()
        {
            byte[] code = { 0x2a, 0x00, 0xa0, 0xe3,      // mov r0, #42
                            0x1e, 0xff, 0x2f, 0xe1 };     // bx lr
            IntPtr p = mmap(IntPtr.Zero, (UIntPtr)PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            int e = Marshal.GetLastWin32Error();
            if (p == MAP_FAILED || p == IntPtr.Zero)
            { W("anon-exec", "mmap(anon rw) branch=failed " + Errno(e)); return "anon-exec: mmap failed(" + e + ")"; }
            try
            {
                Marshal.Copy(code, 0, p, code.Length);
                if (mprotect(p, (UIntPtr)PAGE, PROT_READ | PROT_EXEC) != 0)
                {
                    e = Marshal.GetLastWin32Error();
                    W("anon-exec", "mprotect(rx) branch=failed " + Errno(e));
                    return "anon-exec: mprotect failed(" + e + ")";
                }
                W("anon-exec", "mprotect(rx) branch=ok");
                var fn = (AnonFn)Marshal.GetDelegateForFunctionPointer(p, typeof(AnonFn));
                int r = fn();
                W("anon-exec", "call branch=ok, returned " + r + (r == 42 ? " (expected)" : " (UNEXPECTED)"));
                return r == 42 ? "anon-exec: ok(42)" : "anon-exec: wrong result " + r;
            }
            finally { munmap(p, (UIntPtr)PAGE); }
        }

        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int AnonFn();

        private static string Deps()
        {
            string bad = null;
            foreach (var d in MemLoader.EngineDeps)
            {
                string why;
                IntPtr h = MemLoader.TryOpen(d, out why);
                if (h == IntPtr.Zero) { W("deps", d + " branch=failed: " + why); if (bad == null) bad = d; }
                else { W("deps", d + " branch=ok"); dlclose(h); }
            }
            return "deps: " + (bad == null ? "all load" : "first failing " + bad);
        }
    }
}

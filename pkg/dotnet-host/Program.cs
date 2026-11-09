/*
 * MoonlightHost - Tizen .NET host for the dev-cert path.
 *
 * On a retail Samsung TV only Web (.wgt) and .NET (.tpk) apps install, so
 * the installable unit is a dev-signed .NET .tpk; our native streaming engine ships
 * inside it as libmlengine.so and is driven over the stable C ABI (mlengine.h).
 *
 * The engine is loaded from memory rather than from a file in the app folder: we
 * read libmlengine.so's bytes and load them from an anonymous in-memory fd
 * (memfd_create -> write -> dlopen /proc/self/fd/N); when that fails, it dlopens the
 * packaged file. Only OUR .so comes from
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

    // Loads a shared object from memory (an anonymous memfd).
    internal static class MemLoader
    {
        private const uint MFD_CLOEXEC = 0x0001u;
        private const int  RTLD_NOW    = 0x0002;
        // Tizen 4/5 libc does NOT export memfd_create, so create the anonymous fd by
        // raw syscall. ARM (32-bit) __NR_memfd_create = 385.
        private const int  SYS_memfd_create_arm = 385;

        [DllImport("libc", EntryPoint = "syscall", SetLastError = true)]
        // number is C long = 32-bit on ARM; a C# long (64-bit) shifts name/flags into the wrong registers -> EINVAL.
        private static extern int syscall_memfd(int number, string name, uint flags);
        [DllImport("libc", SetLastError = true)]  private static extern IntPtr write(int fd, IntPtr buf, UIntPtr count);
        [DllImport("libdl.so.2", SetLastError = true)]  private static extern IntPtr dlopen(string filename, int flags);
        [DllImport("libdl.so.2")]                       public  static extern IntPtr dlsym(IntPtr handle, string symbol);
        [DllImport("libdl.so.2")]                       private static extern IntPtr dlerror();

        // Load the engine image from an anonymous fd. The fd must stay open for process life.
        public static IntPtr LoadFromMemory(byte[] so)
        {
            return Open("/proc/self/fd/" + CreateMemfd(so), "memfd");
        }

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
            IntPtr p = dlsym(handle, symbol);
            if (p == IntPtr.Zero) throw new EntryPointNotFoundException("dlsym: " + symbol);
            Log.Trace("dlsym " + symbol + " -> 0x" + p.ToInt64().ToString("x"));
            return (T)(object)Marshal.GetDelegateForFunctionPointer(p, typeof(T));
        }
    }

    // ml_engine_stats (mlengine.h): 15 x 4 bytes, no padding.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EngineStats
    {
        public int StructSize, Running, DepthMs, AudioDepthMs;
        public uint PlayingMs, Submitted, DroppedLate, DroppedFull, FpsSubmitted, Underruns, Overruns, Errors, LastError, Recoveries;
        public int RecoveryState;
    }

    // ml_engine_audio_stats (mlengine.h): 7 x 4 bytes, no padding.
    [StructLayout(LayoutKind.Sequential)]
    internal struct EngineAudioStats
    {
        public int StructSize, Route;
        public uint Frames, Underruns, Overruns, Drops;
        public int BufMs;
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
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetStatsDelegate(ref EngineStats s);
        public static GetStatsDelegate      GetStats;   // null with an engine that lacks it
        [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
        public delegate int  StartSessionV2Delegate(string host, int width, int height, int fps, int bitrateKbps,
                                                    string rikeyHex, int rikeyId, string appVersion,
                                                    string gfeVersion, string rtspUrl,
                                                    int serverCodecModeSupport, string videoCodec, int hdr,
                                                    string audioConfig, int fullRange);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int LastErrorDelegate(byte[] buf, int len);
        public static LastErrorDelegate     LastError;   // null with an engine that lacks it
        public static StartSessionV2Delegate StartSessionV2;   // null with an engine that lacks it
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GamepadArrivalDelegate(int index, int mask, int type, uint buttons, int caps);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GamepadStateDelegate(int index, int mask, int buttons, int lt, int rt, int lsx, int lsy, int rsx, int rsy);
        public static GamepadArrivalDelegate GamepadArrival;   // null with an engine that lacks it
        public static GamepadStateDelegate   GamepadState;     // null with an engine that lacks it
        // Decoded PCM goes to the host (the TV player cannot play engine-fed audio); see AudioOut.
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void AudioCbDelegate(IntPtr pcm, int samples, int channels, int rate);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetAudioCbDelegate(AudioCbDelegate cb);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetAudioDropsDelegate(int dropped);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetAudioHostStatsDelegate(int sent, int dropped, int sendFailures);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetAudioRouteDelegate();
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetAudioStatsDelegate(ref EngineAudioStats s);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void SetGameModeDelegate(int on);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int TvGameModeDelegate(int on, int saveType);
        public static TvGameModeDelegate    TvGameMode;      // null with an engine that lacks it
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int SetStatsOsdDelegate(int on);
        [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate int GetStatsOsdDelegate();
        public static SetStatsOsdDelegate   SetStatsOsd;     // null with an engine that lacks it
        public static GetStatsOsdDelegate   GetStatsOsd;     // null with an engine that lacks it

        public static SetAudioCbDelegate    SetAudioCb;      // null with an engine that lacks it
        public static SetAudioDropsDelegate SetAudioDrops;   // null with an engine that lacks it
        public static SetAudioHostStatsDelegate SetAudioHostStats;   // null with an engine that lacks it
        public static GetAudioRouteDelegate GetAudioRoute;   // null with an engine that lacks it
        public static GetAudioStatsDelegate GetAudioStats;   // null with an engine that lacks it
        public static SetGameModeDelegate   SetGameMode;     // null with an engine that lacks it
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
            try { GetStats = MemLoader.GetDelegate<GetStatsDelegate>(h, "ml_engine_get_stats"); }
            catch (EntryPointNotFoundException) { GetStats = null; }
            try { StartSessionV2 = MemLoader.GetDelegate<StartSessionV2Delegate>(h, "ml_engine_start_session_v2"); }
            catch (EntryPointNotFoundException) { StartSessionV2 = null; }
            try { LastError = MemLoader.GetDelegate<LastErrorDelegate>(h, "ml_engine_last_error"); }
            catch (EntryPointNotFoundException) { LastError = null; }
            try { SetAudioCb = MemLoader.GetDelegate<SetAudioCbDelegate>(h, "ml_engine_set_audio_cb"); }
            catch (EntryPointNotFoundException) { SetAudioCb = null; }
            try { SetAudioDrops = MemLoader.GetDelegate<SetAudioDropsDelegate>(h, "ml_engine_set_audio_drops"); }
            catch (EntryPointNotFoundException) { SetAudioDrops = null; }
            try { SetAudioHostStats = MemLoader.GetDelegate<SetAudioHostStatsDelegate>(h, "ml_engine_set_audio_host_stats"); }
            catch (EntryPointNotFoundException) { SetAudioHostStats = null; }
            try { GetAudioRoute = MemLoader.GetDelegate<GetAudioRouteDelegate>(h, "ml_engine_get_audio_route"); }
            catch (EntryPointNotFoundException) { GetAudioRoute = null; }
            try { GetAudioStats = MemLoader.GetDelegate<GetAudioStatsDelegate>(h, "ml_engine_get_audio_stats"); }
            catch (EntryPointNotFoundException) { GetAudioStats = null; }
            try { SetGameMode = MemLoader.GetDelegate<SetGameModeDelegate>(h, "ml_engine_set_gamemode"); }
            catch (EntryPointNotFoundException) { SetGameMode = null; }
            try { TvGameMode = MemLoader.GetDelegate<TvGameModeDelegate>(h, "ml_engine_tv_game_mode"); }
            catch (EntryPointNotFoundException) { TvGameMode = null; }
            try { SetStatsOsd = MemLoader.GetDelegate<SetStatsOsdDelegate>(h, "ml_engine_set_stats_osd"); }
            catch (EntryPointNotFoundException) { SetStatsOsd = null; }
            try { GetStatsOsd = MemLoader.GetDelegate<GetStatsOsdDelegate>(h, "ml_engine_get_stats_osd"); }
            catch (EntryPointNotFoundException) { GetStatsOsd = null; }
            try { GamepadArrival = MemLoader.GetDelegate<GamepadArrivalDelegate>(h, "ml_engine_gamepad_arrival"); }
            catch (EntryPointNotFoundException) { GamepadArrival = null; }
            try { GamepadState = MemLoader.GetDelegate<GamepadStateDelegate>(h, "ml_engine_gamepad_state"); }
            catch (EntryPointNotFoundException) { GamepadState = null; }
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
        private static System.Threading.Timer s_stats;   // live stats overlay, null while off
        private static readonly object s_statsLock = new object();

        public static void Start()
        {
            s_port = new Tizen.Applications.Messages.MessagePort("moonlight", false);
            s_port.MessageReceived += OnMessage;
            s_port.Listen();
            s_monitor = new System.Threading.Timer(_ => Poll(), null, 500, 500);
            Log.Trace("command port 'moonlight' listening");
        }

        private static readonly object s_engineLock = new object();   // start/stop: command threads and Poll

        private static void Poll()
        {
            try
            {
                if (s_active && Engine.Loaded && Engine.Running() == 0)
                {
                    s_active = false;
                    AudioStop();   // reports the final drop count before the ML-ERROR line is read
                    int code = ReadLastError(out string diag);
                    if (diag != null) Log.Error(diag);
                    SendTerminated("streamTerminated: " + code, diag);
                    // the host dropped the stream: stop the connection now, or its audio keeps
                    // feeding the player and the overlay stays taken until the next start
                    lock (s_engineLock) { if (Engine.Running() == 0) Engine.Stop(); }
                }
            }
            catch (Exception ex) { Log.Error("monitor failed: " + ex.Message); }
        }

        // Engine failure code (0 = none / old engine) and its ML-ERROR line (null when empty).
        private static int ReadLastError(out string line)
        {
            line = null;
            if (!Engine.Loaded || Engine.LastError == null) return 0;
            var buf = new byte[4096];
            int code = Engine.LastError(buf, buf.Length);
            int n = Array.IndexOf(buf, (byte)0);
            if (n < 0) n = buf.Length;
            if (n > 0) line = System.Text.Encoding.UTF8.GetString(buf, 0, n);
            return code;
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
                if (cmd == "gamepadArrival" || cmd == "gamepadState") { Gamepad(b, cmd); return; }   // inline: a task per state would reorder them
                Log.Trace("cmd " + cmd + " id=" + id);
                string replyApp = s_uiApp, replyPort = s_uiPort;
                Task.Run(() => Run(b, cmd ?? "", id, replyApp, replyPort));
            }
            catch (Exception ex) { Log.Error("command receive failed: " + ex.Message); }
        }

        private static bool s_padWarned;   // one log line at most for dropped pad commands

        // Pad input: no reply, runs on the port callback so the order is kept. The engine calls are cheap.
        private static void Gamepad(Tizen.Applications.Bundle b, string cmd)
        {
            bool arrival = cmd == "gamepadArrival";
            string[] keys = arrival ? new[] { "index", "mask", "type", "buttons", "caps" }
                                    : new[] { "index", "mask", "buttons", "lt", "rt", "lsx", "lsy", "rsx", "rsy" };
            var v = new int[keys.Length];
            bool ok = (arrival ? Engine.GamepadArrival : (Delegate)Engine.GamepadState) != null;
            string why = ok ? null : "engine lacks it";
            for (int i = 0; ok && i < keys.Length; i++)
                if (!int.TryParse(Item(b, keys[i]), out v[i])) { ok = false; why = "bad args"; }
            if (!ok)
            {
                if (!s_padWarned) { s_padWarned = true; Log.Error(cmd + " dropped: " + why); }
                return;
            }
            // TryEnter, never a blocking lock: Stop/start hold s_engineLock while they wait for the main loop (this thread).
            // A dropped state is resent by the UI keepalive.
            if (!Monitor.TryEnter(s_engineLock)) return;
            try
            {
                if (arrival) Engine.GamepadArrival(v[0], v[1], v[2], unchecked((uint)v[3]), v[4]);
                else Engine.GamepadState(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8]);
            }
            finally { Monitor.Exit(s_engineLock); }
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
                if (err == null)
                {
                    s_active = true; SendEvent("Connection Established");
                    // The engine picked its audio route at init: start the mlaudio sender now
                    // on the callback route (native output sends no mlaudio events).
                    lock (s_aLock) { if (s_aOn) { int gen = s_aGen; int route = AudioRoute();
                        if (route != 2) AudioSenderStart(gen); else SendAudioRoute(2); } }
                }
                else { AudioStop(); ReadLastError(out string diag); if (diag != null) Log.Error(diag); SendTerminated("streamTerminated: -1", diag); }
            }
        }

        private static bool Send(Tizen.Applications.Bundle msg, string app, string port, bool quiet = false)
        {
            try { s_port.Send(msg, app, port); return true; }
            catch (Exception ex) { if (!quiet) Log.Error("send failed: " + ex.Message); return false; }
        }

        // A message port message is limited to ~4 KB, so a large message travels as
        // ordered parts {id, seq, total, field, chunk} (pkg/ui/PROTOCOL.md). The sizes
        // leave room for the Bundle encoding overhead.
        private const int MaxSingle = 1536, ChunkBytes = 1536, ChunkChars = 512;   // 512 chars <= 1536 UTF-8 bytes

        private static bool SendReply(string id, Dictionary<string, object> r, string app, string port, bool quiet = false)
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
                return Send(m, app, port, quiet);
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
                if (!Send(m, app, port, quiet)) return false;   // the UI times out the command
            }
            if (!quiet) Log.Trace("reply " + id + " sent in " + parts.Count + " parts");
            return true;
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

        private static void SendTerminated(string ev, string diag = null)
        {
            StopStats();
            SendEvent(ev, diag);
        }

        private static void StopStats()
        {
            lock (s_statsLock) { s_stats?.Dispose(); s_stats = null; }
        }

        // on: "true"/"false" = the state the UI wants (a lost reply then cannot invert it);
        // absent = flip. Returns an error text, or null when ok.
        private static string ToggleStats(string on, Dictionary<string, object> r)
        {
            if (Engine.GetStats == null) return "not supported";
            bool want;
            lock (s_statsLock)
            {
                want = on == null ? s_stats == null : on == "true";
                if (!want) { s_stats?.Dispose(); s_stats = null; }
                else if (s_stats == null) s_stats = new System.Threading.Timer(_ => StatsTick(), null, 500, 500);
            }
            if (Engine.SetStatsOsd != null)   // outside s_statsLock: the engine call may wait for the main loop
            {
                int rc = Engine.SetStatsOsd(want ? 1 : 0);
                int win = Engine.GetStatsOsd != null ? Engine.GetStatsOsd() : 0;
                Log.Info("stats osd " + want + " -> " + rc + " win " + win);
                r.Add("osd", rc.ToString());
                if (Engine.GetStatsOsd != null) r.Add("osdWin", win.ToString());
            }
            return null;
        }

        private static void StatsTick()
        {
            try
            {
                if (!s_active) return;   // no session: the counters would be stale
                var s = new EngineStats { StructSize = Marshal.SizeOf<EngineStats>() };
                if (Engine.GetStats(ref s) != 0) return;
                var c = System.Globalization.CultureInfo.InvariantCulture;
                var m = new Tizen.Applications.Bundle();
                m.AddItem("event", "mlstats");
                m.AddItem("depth", s.DepthMs.ToString(c));
                m.AddItem("fps", s.FpsSubmitted.ToString(c));
                m.AddItem("drop", s.DroppedLate.ToString(c));
                m.AddItem("full", s.DroppedFull.ToString(c));
                m.AddItem("uo", s.Underruns.ToString(c) + "/" + s.Overruns.ToString(c));
                m.AddItem("err", s.Errors.ToString(c));
                m.AddItem("rec", s.Recoveries.ToString(c));
                m.AddItem("play", s.PlayingMs.ToString(c));
                long aSent, aDrop; lock (s_aLock) { aSent = s_aSent; aDrop = s_aDropped; }
                m.AddItem("asent", aSent.ToString(c));
                m.AddItem("adrop", aDrop.ToString(c));
                m.AddItem("afail", Interlocked.Read(ref s_aFailed).ToString(c));
                if (Engine.GetAudioStats != null)   // native audio counters (new engine)
                {
                    var a = new EngineAudioStats { StructSize = Marshal.SizeOf<EngineAudioStats>() };
                    if (Engine.GetAudioStats(ref a) == 0 && a.Route == 2)
                    {
                        m.AddItem("aroute", "native");
                        m.AddItem("aframes", a.Frames.ToString(c));
                        m.AddItem("auo", a.Underruns.ToString(c) + "/" + a.Overruns.ToString(c));
                        m.AddItem("adrops", a.Drops.ToString(c));
                        m.AddItem("abuf", a.BufMs.ToString(c));
                    }
                }
                Send(m, s_uiApp, s_uiPort);
            }
            catch (Exception ex) { Log.Error("stats failed: " + ex.Message); }
        }

        private static int s_eventSeq;

        // ---- Audio: engine callback -> bounded queue -> sender thread -> "mlaudio" events ----
        // The callback (engine audio thread) only copies and enqueues under a short lock.
        // Queue holds at most 500 ms; on overflow the oldest frame goes (live audio).
        private sealed class AFrame { public int Rate, Ch, Samples; public byte[] Data; }
        private static readonly Engine.AudioCbDelegate s_audioCb = OnAudio;   // kept alive for native code
        private static readonly object s_aLock = new object();
        private static readonly Queue<AFrame> s_aQueue = new Queue<AFrame>();
        private static double s_aQueuedMs;
        private static int s_aGen;                       // session generation; a sender of an older one exits
        private static volatile bool s_aOn;
        private static bool s_aSenderOn;                 // the mlaudio sender thread runs (callback route)
        private static int s_aRouteLast = -1;            // last route sent to the UI (mlaroute); -1 = none yet
        private static bool s_aSkipLogged;
        private static readonly Stack<byte[]> s_aFree = new Stack<byte[]>();   // under s_aLock; exact-size frame buffers
        private const int AudioFreeMax = 16;
        private static long s_aSent, s_aDropped, s_aFailed;   // drops/sent under s_aLock, failed on the sender thread
        private static System.Threading.Timer s_aTimer;   // under s_aLock
        private static bool s_aWarned;
        private const double AudioMaxMs = 500;

        private static void OnAudio(IntPtr pcm, int samples, int channels, int rate)
        {
            try   // never throw or block in native code
            {
                if (pcm == IntPtr.Zero || samples <= 0 || channels <= 0 || rate <= 0 || samples * channels > 48000) return;
                if (!s_aOn) return;   // no copy when audio is off
                int len = samples * channels * 2;
                lock (s_aLock)
                {
                    if (!s_aOn) return;
                    byte[] buf = null;
                    while (s_aFree.Count > 0 && buf == null) { var t = s_aFree.Pop(); if (t.Length == len) buf = t; }   // other size: drop
                    if (buf == null) buf = new byte[len];
                    Marshal.Copy(pcm, buf, 0, len);   // S16LE on the little-endian TV
                    var f = new AFrame { Rate = rate, Ch = channels, Samples = samples, Data = buf };
                    s_aQueue.Enqueue(f);
                    s_aQueuedMs += 1000.0 * samples / rate;
                    while (s_aQueuedMs > AudioMaxMs && s_aQueue.Count > 1)
                    {
                        var o = s_aQueue.Dequeue();
                        s_aQueuedMs -= 1000.0 * o.Samples / o.Rate;
                        s_aDropped++;
                        if (s_aFree.Count < AudioFreeMax) s_aFree.Push(o.Data);
                    }
                    Monitor.Pulse(s_aLock);
                }
            }
            catch { }
        }

        // The engine's audio route (mlengine.h): 0 none, 1 callback (this host sends mlaudio
        // events), 2 native audio-io in the engine (no mlaudio events). An old engine has
        // no ml_engine_get_audio_route: the callback path is the only one, so 1.
        private static int AudioRoute()
        {
            try { if (Engine.GetAudioRoute != null) return Engine.GetAudioRoute(); }
            catch (Exception ex) { Log.Error("audio route read failed: " + ex.Message); }
            return 1;
        }

        private static void SendAudioRoute(int route)
        {
            try
            {
                var m = new Tizen.Applications.Bundle();
                m.AddItem("event", "mlaroute");
                m.AddItem("route", route == 2 ? "native" : route == 1 ? "webaudio" : "none");
                Send(m, s_uiApp, s_uiPort);
            }
            catch (Exception ex) { Log.Error("audio route send failed: " + ex.Message); }
        }

        private static void AudioStart()
        {
            if (Engine.SetAudioCb == null)
            {
                if (!s_aWarned) { s_aWarned = true; Log.Info("audio output unavailable (old engine)"); }
                return;
            }
            int gen;
            lock (s_aLock)
            {
                s_aQueue.Clear(); s_aQueuedMs = 0;
                s_aSent = s_aDropped = s_aFailed = 0;
                s_aFree.Clear();
                s_aOn = true;
                gen = ++s_aGen;
            }
            try { PushAudioStats(0, 0, 0); Engine.SetAudioCb(s_audioCb); }
            catch (Exception ex) { Log.Error("audio callback set failed: " + ex.Message); }
            // The engine picks its route at audio init (after Connection Established): the
            // callback must be registered by then, but the sender only runs on the callback
            // route. ReportDrops re-checks: a mid-session fallback to the callback starts it.
            s_aSenderOn = false;
            lock (s_aLock)   // AudioStop may run on another thread (Poll) at the same time
            {
                s_aTimer?.Dispose();
                s_aTimer = new System.Threading.Timer(_ => ReportDrops(), null, 1000, 1000);
            }
            Log.Info("audio output start");
        }

        // Starts the mlaudio sender (called when the route is / becomes the callback path).
        private static void AudioSenderStart(int gen)
        {
            lock (s_aLock)
            {
                if (!s_aOn || gen != s_aGen || s_aSenderOn) return;
                s_aSenderOn = true;
            }
            new Thread(() => AudioSender(gen)) { IsBackground = true, Name = "mlaudio" }.Start();
        }

        // New engines take sent/dropped/failed; an older one only the drop count.
        private static void PushAudioStats(long sent, long dropped, long failed)
        {
            int i(long v) { return (int)Math.Min(v, int.MaxValue); }
            if (Engine.SetAudioHostStats != null) Engine.SetAudioHostStats(i(sent), i(dropped), i(failed));
            else Engine.SetAudioDrops?.Invoke(i(dropped));
        }

        private static void ReportDrops()
        {
            try
            {
                long s, d; int gen;
                lock (s_aLock) { if (!s_aOn) return; s = s_aSent; d = s_aDropped; gen = s_aGen; }
                int route = AudioRoute();
                if (route != 2) AudioSenderStart(gen);   // callback route (or none): the sender is the fallback path
                if (route != s_aRouteLast)
                {
                    s_aRouteLast = route;
                    if (route == 2) Log.Info("audio route: native audio-io");
                    SendAudioRoute(route);
                }
                PushAudioStats(s, d, Interlocked.Read(ref s_aFailed));
            }
            catch (Exception ex) { Log.Error("audio drops report failed: " + ex.Message); }
        }

        private static void AudioStop()
        {
            long sent, dropped, failed;
            lock (s_aLock)
            {
                if (!s_aOn) return;   // not running (also: a second stop path)
                s_aOn = false;
                s_aSenderOn = false;
                s_aRouteLast = -1;
                s_aGen++;
                s_aQueue.Clear(); s_aQueuedMs = 0; s_aFree.Clear();
                sent = s_aSent; dropped = s_aDropped; failed = Interlocked.Read(ref s_aFailed);
                Monitor.PulseAll(s_aLock);
                s_aTimer?.Dispose(); s_aTimer = null;   // Dispose does not wait for a running ReportDrops
            }
            try { PushAudioStats(sent, dropped, failed); }
            catch (Exception ex) { Log.Error("audio drops report failed: " + ex.Message); }
            Log.Info("audio output stop: sent=" + sent + " dropped=" + dropped + " sendFailures=" + failed);
        }

        private static void AudioSender(int gen)
        {
            var c = System.Globalization.CultureInfo.InvariantCulture;
            long failedSeq = 0;
            while (true)
            {
                AFrame f;
                lock (s_aLock)
                {
                    while (gen == s_aGen && s_aQueue.Count == 0) Monitor.Wait(s_aLock);
                    if (gen != s_aGen) return;
                    f = s_aQueue.Dequeue();
                    s_aQueuedMs -= 1000.0 * f.Samples / f.Rate;
                }
                bool ok = false;
                try
                {
                    var r = new Dictionary<string, object>
                    {
                        { "event", "mlaudio" }, { "rate", f.Rate.ToString(c) }, { "ch", f.Ch.ToString(c) },
                        { "samples", f.Samples.ToString(c) }, { "data", f.Data }
                    };
                    ok = SendReply("au" + Interlocked.Increment(ref s_eventSeq), r, s_uiApp, s_uiPort, true);
                }
                catch { }
                lock (s_aLock)
                {
                    if (ok && gen == s_aGen) s_aSent++;
                    if (gen == s_aGen && s_aFree.Count < AudioFreeMax) s_aFree.Push(f.Data);   // the send is done with the array
                }
                if (!ok) { Interlocked.Increment(ref s_aFailed); if (++failedSeq == 1) Log.Error("audio send failed (further failures only counted)"); }
            }
        }

        private static void SendEvent(string ev, string diag = null)
        {
            if (!string.IsNullOrEmpty(diag))
            {
                // the ML-ERROR line can pass the ~4 KB message limit: send it as parts (own id, never a command id)
                var r = new Dictionary<string, object> { { "event", ev }, { "diag", diag } };
                SendReply("ev" + Interlocked.Increment(ref s_eventSeq), r, s_uiApp, s_uiPort);
                return;
            }
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
                        int fullRange = Item(b, "fullRange") == "true" ? 1 : 0;   // optional: absent = limited
                        Engine.SetGameMode?.Invoke(Item(b, "gameMode") == "true" ? 1 : 0);   // optional: absent = off
                        lock (s_engineLock)
                        {
                            if (s_active && Engine.Running() != 0)   // already streaming: keep the audio path as it is
                            {
                                if (!s_aSkipLogged) { s_aSkipLogged = true; Log.Info("audio output start skipped: a stream is running"); }
                            }
                            else AudioStart();
                            if (Engine.StartSessionV2 != null)
                                return Rc(Engine.StartSessionV2(Item(b, "host"), w, h, fps, br, Item(b, "rikey"), rikeyid,
                                                                Item(b, "appVer"), Item(b, "gfeVer"), Item(b, "rtspUrl"),
                                                                codec, Item(b, "videoCodec"), Item(b, "hdr") == "true" ? 1 : 0,
                                                                Item(b, "audioConfig"), fullRange));
                            return Rc(Engine.StartSession(Item(b, "host"), w, h, fps, br, Item(b, "rikey"), rikeyid,
                                                          Item(b, "appVer"), Item(b, "gfeVer"), Item(b, "rtspUrl"),
                                                          codec, Item(b, "videoCodec"), Item(b, "hdr") == "true" ? 1 : 0,
                                                          Item(b, "audioConfig")));
                        }
                    }
                case "stopStream": lock (s_engineLock) Engine.Stop(); AudioStop(); return null;
                case "sendKeyboardEvent":
                    {
                        int key, action, mods;
                        if (!int.TryParse(Item(b, "key"), out key) || !int.TryParse(Item(b, "action"), out action) ||
                            !int.TryParse(Item(b, "mods"), out mods)) return "bad args";
                        Engine.SendKey(key, action, mods);
                        return null;
                    }
                case "tvGameMode":
                    {
                        int on = Item(b, "on") == "true" ? 1 : 0;
                        int saveType;
                        if (!int.TryParse(Item(b, "saveType"), out saveType)) saveType = 1;   // optional: the session's value
                        if (Engine.TvGameMode == null) return "not supported";
                        int rc;
                        lock (s_engineLock) rc = Engine.TvGameMode(on, saveType);
                        Log.Info("tv game mode " + on + " saveType " + saveType + " -> " + rc);
                        r.Add("rc", rc.ToString());
                        return null;
                    }
                case "statsOsd":
                    {
                        bool on = Item(b, "on") == "true";
                        if (Engine.SetStatsOsd == null) return "not supported";
                        int rc = Engine.SetStatsOsd(on ? 1 : 0);   // no lock held: the engine call may wait for the main loop
                        int win = Engine.GetStatsOsd != null ? Engine.GetStatsOsd() : 0;
                        Log.Info("stats osd " + on + " -> " + rc + " win " + win);
                        r.Add("rc", rc.ToString());
                        if (Engine.GetStatsOsd != null) r.Add("win", win.ToString());
                        return null;
                    }
                case "toggleStats": return ToggleStats(Item(b, "on"), r);
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

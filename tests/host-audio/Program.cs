using System;
using System.Collections.Generic;
using System.Linq;
using MoonlightHost;

// Host-side audio batching helpers (pkg/dotnet-host/AudioBatch.cs). Exit code 1 on failure.
internal static class Test
{
    private static int fail;
    private static void Check(bool ok, string what) { if (!ok) { fail++; Console.WriteLine("FAIL " + what); } else Console.WriteLine("ok   " + what); }

    private static int Main()
    {
        Check(AudioBatch.ParseBatchMs(null) == 20, "null -> 20");
        Check(AudioBatch.ParseBatchMs("abc") == 20, "garbage -> 20");
        Check(AudioBatch.ParseBatchMs("") == 20, "empty -> 20");
        Check(AudioBatch.ParseBatchMs("-5") == 0, "-5 -> 0");
        Check(AudioBatch.ParseBatchMs("500") == 100, "500 -> 100");
        Check(AudioBatch.ParseBatchMs("0") == 0, "0 -> 0");
        Check(AudioBatch.ParseBatchMs("35") == 35, "35 -> 35");

        var buf = new byte[3840];
        for (int i = 0; i < buf.Length; i++) buf[i] = (byte)(i * 7);
        var parts = AudioBatch.Split(buf, buf.Length, 1536);
        Check(parts.Count == 3 && parts[0].Length == 1536 && parts[1].Length == 1536 && parts[2].Length == 768, "3840 B -> 1536/1536/768");
        Check(parts.SelectMany(p => p).SequenceEqual(buf), "parts re-concatenate to the input");
        parts = AudioBatch.Split(buf, 960, 1536);
        Check(parts.Count == 1 && parts[0].Length == 960 && parts[0].SequenceEqual(buf.Take(960)), "960 B -> one part (len, not buffer size)");

        Check(AudioBatch.TakeNext(48000, 2, 5, 48000, 2, 20), "same format, below limit: take");
        Check(!AudioBatch.TakeNext(48000, 2, 5, 44100, 2, 20), "rate change: stop");
        Check(!AudioBatch.TakeNext(48000, 2, 5, 48000, 6, 20), "channel change: stop");
        Check(!AudioBatch.TakeNext(48000, 2, 20, 48000, 2, 20), "limit reached: stop");
        Check(!AudioBatch.TakeNext(48000, 2, 5, 48000, 2, 0), "batchMs 0: per frame");
        CollectTests();
        return fail == 0 ? 0 : 1;
    }

    private static AFrame F(int t, int rate = 48000, int ch = 2) { return new AFrame { Rate = rate, Ch = ch, Samples = rate / 200, T = t, Data = new byte[1] }; }

    private static void CollectTests()
    {
        int clock = 0, appended = 0, waits = 0, lastWait = 0;
        Func<int> now = () => clock;
        Action<AFrame> app = f => appended++;

        // 1: full batch from the queue, no wait
        clock = 1000; appended = 0;
        var q = new Queue<AFrame>(); for (int i = 0; i < 6; i++) q.Enqueue(F(1000));
        bool r = AudioBatch.Collect(q, F(1000), 20, app, now, w => { throw new Exception("wait called"); });
        Check(r && appended == 3 && q.Count == 3, "full batch: 3 more taken, rest queued, no wait (queued=" + q.Count + ")");

        // 2: timeout flush, requested timeout = batchMs - elapsed
        clock = 1007; appended = 0; waits = 0; lastWait = -1; q = new Queue<AFrame>();
        r = AudioBatch.Collect(q, F(1000), 20, app, now, w => { waits++; lastWait = w; clock += w; return true; });
        Check(r && appended == 0 && waits == 1 && lastWait == 13, "timeout flush: wait(13) then send (wait=" + lastWait + ")");

        // 3: a frame arrives during the wait
        clock = 1000; appended = 0; waits = 0; q = new Queue<AFrame>();
        r = AudioBatch.Collect(q, F(1000), 20, app, now, w => { waits++; if (waits == 1) { q.Enqueue(F(1005)); clock += 5; } else clock += w; return true; });
        Check(r && appended == 1 && waits == 2, "frame during wait is taken, then timeout (appended=" + appended + " waits=" + waits + ")");

        // 4: rate change, channel change
        foreach (var other in new[] { F(1000, 44100, 2), F(1000, 48000, 6) })
        {
            clock = 1000; appended = 0; q = new Queue<AFrame>(); q.Enqueue(other); q.Enqueue(F(1000));
            r = AudioBatch.Collect(q, F(1000), 20, app, now, w => { throw new Exception("wait called"); });
            Check(r && appended == 0 && q.Count == 2 && q.Peek() == other, "format change flushes, queue unchanged (rate " + other.Rate + " ch " + other.Ch + ")");
        }

        // 5: stop
        clock = 1000; appended = 0; q = new Queue<AFrame>();
        r = AudioBatch.Collect(q, F(1000), 20, app, now, w => false);
        Check(!r, "wait returns false: Collect returns false");

        // 6: batchMs 0
        clock = 1000; appended = 0; q = new Queue<AFrame>(); q.Enqueue(F(1000));
        r = AudioBatch.Collect(q, F(1000), 0, app, now, w => { throw new Exception("wait called"); });
        Check(r && appended == 0 && q.Count == 1, "batchMs 0: nothing taken, no wait");

        // 7: TickCount wrap
        clock = int.MinValue + 2; lastWait = -1; q = new Queue<AFrame>();
        r = AudioBatch.Collect(q, F(int.MaxValue - 2), 20, app, now, w => { lastWait = w; clock += w; return true; });
        Check(r && lastWait == 15, "TickCount wrap (5 ms elapsed): wait(15) (wait=" + lastWait + ")");
    }
}

using System;
using System.Collections.Generic;

namespace MoonlightHost
{
    internal sealed class AFrame { public int Rate, Ch, Samples, T; public byte[] Data; }   // T: TickCount at enqueue

    // Pure helpers for the audio sender (no Tizen types, so a host test can compile them). C# 7.3.
    internal static class AudioBatch
    {
        public const int DefaultMs = 20, MaxMs = 100;

        // null/garbage -> default; otherwise clamped to 0..MaxMs.
        public static int ParseBatchMs(string s)
        {
            int v;
            if (!int.TryParse(s, System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out v)) return DefaultMs;
            return Math.Max(0, Math.Min(MaxMs, v));
        }

        // Another frame joins the batch: same format and the batch is still shorter than batchMs.
        public static bool TakeNext(int batchRate, int batchCh, double batchedMs, int rate, int ch, int batchMs)
        {
            return rate == batchRate && ch == batchCh && batchedMs < batchMs;
        }

        // The caller holds its lock and has already dequeued and appended `first`. Takes more frames
        // from q until the batch is batchMs long, the format changes, or the oldest frame is batchMs old.
        // wait(ms) releases the lock for at most ms; false = the session stopped. true = send the batch.
        public static bool Collect(Queue<AFrame> q, AFrame first, int batchMs, Action<AFrame> append, Func<int> now, Func<int, bool> wait)
        {
            double ms = 1000.0 * first.Samples / first.Rate;
            while (TakeNext(first.Rate, first.Ch, ms, first.Rate, first.Ch, batchMs))
            {
                if (q.Count > 0)
                {
                    var n = q.Peek();
                    if (!TakeNext(first.Rate, first.Ch, ms, n.Rate, n.Ch, batchMs)) return true;
                    q.Dequeue();
                    append(n);
                    ms += 1000.0 * n.Samples / n.Rate;
                }
                else
                {
                    int remaining = batchMs - unchecked(now() - first.T);
                    if (remaining <= 0) return true;
                    if (!wait(remaining)) return false;
                }
            }
            return true;
        }

        public static List<byte[]> Split(byte[] buf, int len, int chunk)
        {
            var parts = new List<byte[]>();
            int off = 0;
            do
            {
                int n = Math.Min(chunk, len - off);
                var c = new byte[n];
                Array.Copy(buf, off, c, 0, n);
                parts.Add(c);
                off += n;
            } while (off < len);
            return parts;
        }
    }
}

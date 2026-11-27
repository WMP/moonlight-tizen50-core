/*
 * audio_opus.c - moonlight-common-c AUDIO_RENDERER_CALLBACKS. The host's Opus frames are
 * decoded in the engine with libopus (fixed point, from the submodule; libopus_tv.a) to S16LE
 * PCM. Two output paths:
 *
 *  - native (preferred): the TV's capi-media-audio-io (audio_out_*), dlopen'd at audio init
 *    (never a link-time dependency: the engine must load on TVs without it). The decoded
 *    frames go into a small ring buffer drained by a worker thread that calls the blocking
 *    audio_out_write; decodeAndPlaySample runs on moonlight-common-c's receive thread
 *    (CAPABILITY_DIRECT_SUBMIT) and never blocks. The buffer target is ~50 ms; when it grows
 *    past ~2x that, the oldest data is dropped to bound the latency. When create, prepare
 *    or the first writes fail, the session falls back to the callback path and says why.
 *
 *  - callback (fallback, the old path): ml_engine_set_audio_cb (mlengine.h); the host sends
 *    the PCM to the web UI, which plays it with Web Audio (pkg/ui/mlaudio.js).
 *
 * Platform limitation: the TV player rejects engine-fed audio streams, so the esplusplayer
 * carries only video (esplus_player.c) and audio does not share its clock.
 * Without any output at init the stream goes on without sound: init still returns 0 and
 * decodeAndPlaySample returns at once (no decode: nobody could play it).
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dlfcn.h>

#include <opus_multistream.h>

#include "Limelight.h"
#ifdef ML_ENABLE_AC3
#include "audio_ac3.h"
extern void esplus_prepare(void);
#endif
#include "errors.h"
#include "esplus_stats.h"
#include "mldiag.h"
#include "mlengine.h"
#include "mltrace.h"

#define ALOG(...) fprintf(stderr, "[audio] " __VA_ARGS__)

static ml_engine_audio_cb s_cb_set;   /* set by the host, read once per session at init (atomic) */
void ml_engine_set_audio_cb(ml_engine_audio_cb cb) { __atomic_store_n(&s_cb_set, cb, __ATOMIC_RELEASE); }
void ml_engine_set_audio_drops(int dropped) { mldiag_adrops(dropped); }
void ml_engine_set_audio_host_stats(int sent, int dropped, int send_failures) { mldiag_ahost(sent, dropped, send_failures); }

#ifdef ML_ENABLE_AC3
static int s_ac3;
#endif
static int s_samples_per_frame = 240, s_channels = 2, s_rate = 48000;
static OpusMSDecoder *s_dec;
static opus_int16 *s_pcm;   /* one frame, reused: the callback copies it */
static ml_engine_audio_cb s_cb;   /* the callback of this session; NULL = no callback output */
static unsigned s_nerr;

/* ---- native audio-io (capi-media-audio-io, dlopen'd) ---- */

/* audio_io.h enums we need (values from the Tizen 5.0 header; never changed ABI) */
enum { ML_AUDIO_CHANNEL_MONO = 0x80, ML_AUDIO_CHANNEL_STEREO = 0x81 };
enum { ML_AUDIO_SAMPLE_TYPE_S16_LE = 0x71 };
typedef struct ml_audio_out_s *ml_audio_out_h;
typedef int (*ml_aio_create_fn)(int sample_rate, int channel, int type, ml_audio_out_h *output);
typedef int (*ml_aio_prepare_fn)(ml_audio_out_h output);
typedef int (*ml_aio_unprepare_fn)(ml_audio_out_h output);
typedef int (*ml_aio_destroy_fn)(ml_audio_out_h output);
typedef int (*ml_aio_write_fn)(ml_audio_out_h output, void *buffer, unsigned int length);
typedef int (*ml_aio_get_buffer_size_fn)(ml_audio_out_h output, int *size);

/* test seam (tests/audio_opus_test.c): -DML_AUDIO_IO_FAKE replaces dlopen/dlsym with fakes */
#ifdef ML_AUDIO_IO_FAKE
ml_aio_create_fn ml_aio_create;
ml_aio_prepare_fn ml_aio_prepare;
ml_aio_unprepare_fn ml_aio_unprepare;
ml_aio_destroy_fn ml_aio_destroy;
ml_aio_write_fn ml_aio_write;
ml_aio_get_buffer_size_fn ml_aio_get_buffer_size;
#else
static ml_aio_create_fn ml_aio_create;
static ml_aio_prepare_fn ml_aio_prepare;
static ml_aio_unprepare_fn ml_aio_unprepare;
static ml_aio_destroy_fn ml_aio_destroy;
static ml_aio_write_fn ml_aio_write;
static ml_aio_get_buffer_size_fn ml_aio_get_buffer_size;
#endif

#ifndef ML_AUDIO_IO_FAKE
static void *s_aio_lib;              /* dlopen handle, cached for the process */
#endif
static int s_aio_tried;              /* dlopen attempted once */
static char s_aio_reason[64] = "-";  /* why native output is not used (ML-ERROR, ALOG) */

static pthread_mutex_t s_nmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_ncond = PTHREAD_COND_INITIALIZER;
static pthread_t s_nthread;
static int s_nthread_up;             /* worker thread running */
static ml_audio_out_h s_nout;        /* audio_out handle of the session (NULL = no native output) */
static int s_native_on;              /* this session plays natively */
static int s_nrate, s_nch;           /* stream format of the session */

/* ring buffer (byte granular; one frame in, the worker drains it) */
static unsigned char *s_ring;
static size_t s_ring_cap;            /* power of two, bytes */
static size_t s_ring_r, s_ring_w;    /* positions (mod cap) */
static size_t s_ring_fill;           /* bytes in the buffer (invariant: <= cap) */
static unsigned s_n_frames, s_n_under, s_n_over, s_n_drops;   /* counters (worker + receive thread) */
static unsigned s_n_write_calls;     /* audio_out_write calls that returned >= 0 */

#define NATIVE_TARGET_MS 50     /* ring target fill */
#define NATIVE_MAX_MS 100       /* 2x target: above this the oldest data goes */

/* Push the counters to mldiag (ML-ERROR aout section) and to the live stats overlay
   (esplus_stats, ml_engine_get_stats). Cheap; called on the worker path and at cleanup.
   Caller holds s_nmu or is the only thread alive. */
static void native_report_locked(void) {
  int ms = s_nrate > 0 && s_nch > 0 ? (int)(s_ring_fill * 1000 / ((size_t)s_nrate * (size_t)s_nch * 2)) : 0;
  mldiag_anative(s_n_frames, s_n_under, s_n_over, s_n_drops, ms);
  esplus_stats.aout_route = 2;
  esplus_stats.aout_frames = s_n_frames;
  esplus_stats.aout_underruns = s_n_under;
  esplus_stats.aout_overruns = s_n_over;
  esplus_stats.aout_drops = s_n_drops;
  esplus_stats.aout_buf_ms = ms;
}

/* Resolve the audio-io symbols once per process. Returns 0 when they are all there.
   The fake seam (tests): with ML_AUDIO_IO_FAKE the test sets the ml_aio_* pointers and
   pre-marks s_aio_tried, so dlopen is never called. */
static int aio_resolve(void) {
#ifdef ML_AUDIO_IO_FAKE
  if (!s_aio_tried) { s_aio_tried = 1; snprintf(s_aio_reason, sizeof s_aio_reason, "-"); }
  if (!ml_aio_create || !ml_aio_prepare || !ml_aio_write || !ml_aio_unprepare || !ml_aio_destroy) {
    snprintf(s_aio_reason, sizeof s_aio_reason, "no-symbols");
    return -1;
  }
  return 0;
#else
  if (!s_aio_tried) {
    s_aio_tried = 1;
    s_aio_lib = dlopen("libcapi-media-audio-io.so.0", 2 /* RTLD_NOW */);
    if (!s_aio_lib) s_aio_lib = dlopen("libcapi-media-audio-io.so", 2 /* RTLD_NOW */);
    if (s_aio_lib) {
      ml_aio_create = (ml_aio_create_fn)dlsym(s_aio_lib, "audio_out_create_new");
      ml_aio_prepare = (ml_aio_prepare_fn)dlsym(s_aio_lib, "audio_out_prepare");
      ml_aio_unprepare = (ml_aio_unprepare_fn)dlsym(s_aio_lib, "audio_out_unprepare");
      ml_aio_destroy = (ml_aio_destroy_fn)dlsym(s_aio_lib, "audio_out_destroy");
      ml_aio_write = (ml_aio_write_fn)dlsym(s_aio_lib, "audio_out_write");
      ml_aio_get_buffer_size = (ml_aio_get_buffer_size_fn)dlsym(s_aio_lib, "audio_out_get_buffer_size");
    }
    MLT("audio-io: dlopen %s, symbols %s", s_aio_lib ? "ok" : "failed",
        (ml_aio_create && ml_aio_prepare && ml_aio_write && ml_aio_destroy && ml_aio_unprepare) ? "ok" : "missing");
    if (!ml_aio_create || !ml_aio_prepare || !ml_aio_write || !ml_aio_destroy || !ml_aio_unprepare) {
      snprintf(s_aio_reason, sizeof s_aio_reason, "%s", s_aio_lib ? "no-symbols" : "no-library");
      return -1;
    }
  }
  return (ml_aio_create && ml_aio_prepare && ml_aio_write) ? 0 : -1;
#endif
}

/* The worker: blocks in audio_out_write, drains the ring. Runs for the whole session. */
/* The native path ended mid-session: report the fallback (ML-ERROR aout section, stats
   route). Called after s_native_on is cleared and s_aio_reason is set. */
static void native_fell_back(void) {
  mldiag_aout(s_cb != NULL);   /* which output the rest of the session uses */
  mldiag_aout_native(0);
  mldiag_aout_reason(s_aio_reason);
  esplus_stats.aout_route = s_cb ? 1 : 0;
}

static void *native_worker(void *unused) {
  (void)unused;
  int bufsize = 0;
  if (ml_aio_get_buffer_size) ml_aio_get_buffer_size(s_nout, &bufsize);   /* hint only */
  MLT("audio-io worker: start (device buffer %d B)", bufsize);
  int prepared = 0, was_empty = 0;
  for (;;) {
    unsigned char chunk[4096];
    size_t n;
    pthread_mutex_lock(&s_nmu);
    for (;;) {
      if (!s_native_on) { pthread_mutex_unlock(&s_nmu); goto out; }
      if (s_ring_fill > 0) break;
      if (!was_empty) { was_empty = 1; s_n_under++; }   /* the ring ran dry: the device starves */
      pthread_cond_wait(&s_ncond, &s_nmu);
    }
    was_empty = 0;
    n = s_ring_fill < sizeof chunk ? s_ring_fill : sizeof chunk;
    for (size_t i = 0; i < n; i++) chunk[i] = s_ring[(s_ring_r + i) & (s_ring_cap - 1)];
    s_ring_r = (s_ring_r + n) & (s_ring_cap - 1);
    s_ring_fill -= n;
    int stopped = !s_native_on;
    pthread_mutex_unlock(&s_nmu);
    if (stopped) goto out;

    if (!prepared) {   /* prepare once, on the worker: a failure falls back */
      int rc = ml_aio_prepare ? ml_aio_prepare(s_nout) : -1;
      if (rc != 0) {
        pthread_mutex_lock(&s_nmu);
        s_native_on = 0;
        snprintf(s_aio_reason, sizeof s_aio_reason, "prepare-%d", rc);
        pthread_cond_broadcast(&s_ncond);
        pthread_mutex_unlock(&s_nmu);
        ALOG("audio_out_prepare failed %d: falling back to the callback path\n", rc);
        MLT("audio-io prepare -> %d (fallback)", rc);
        native_fell_back();
        goto out;
      }
      prepared = 1;
    }
    int w = ml_aio_write(s_nout, chunk, (unsigned int)n);   /* may block */
    pthread_mutex_lock(&s_nmu);
    if (w < 0) {
      s_native_on = 0;
      snprintf(s_aio_reason, sizeof s_aio_reason, "write-%d", w);
      pthread_cond_broadcast(&s_ncond);
      pthread_mutex_unlock(&s_nmu);
      ALOG("audio_out_write failed %d: falling back to the callback path\n", w);
      MLT("audio-io write -> %d (fallback)", w);
      native_fell_back();
      goto out;
    }
    s_n_write_calls++;
    if (w > 0 && (size_t)w < n) {   /* partial write: give the rest back (rare) */
      s_ring_r = (s_ring_r + s_ring_cap - ((n - w) & (s_ring_cap - 1))) & (s_ring_cap - 1);
      s_ring_fill += n - w;
    }
    pthread_mutex_unlock(&s_nmu);
  }
out: {
    pthread_mutex_lock(&s_nmu);
    if (s_native_on) native_report_locked();   /* a fallback already reported its own route */
    else if (s_n_frames > esplus_stats.aout_frames) {   /* keep the last counters, but not a stale route */
      esplus_stats.aout_frames = s_n_frames; esplus_stats.aout_underruns = s_n_under;
      esplus_stats.aout_overruns = s_n_over; esplus_stats.aout_drops = s_n_drops;
    }
    pthread_mutex_unlock(&s_nmu);
  }
  MLT("audio-io worker: end (writes=%u)", s_n_write_calls);
  return NULL;
}

/* Tear the native path down (idempotent). keep_reason: s_aio_reason survives for the report. */
static void native_stop(void) {
  pthread_t th = 0;
  int had_thread = 0, had_out = 0;
  pthread_mutex_lock(&s_nmu);
  if (s_native_on) {
    s_native_on = 0;
    pthread_cond_broadcast(&s_ncond);
  }
  if (s_nthread_up) { th = s_nthread; had_thread = 1; }
  s_nthread_up = 0;
  had_out = s_nout != NULL;
  pthread_mutex_unlock(&s_nmu);
  if (had_thread) pthread_join(th, NULL);
  if (had_out && s_nout) {
    if (ml_aio_unprepare) ml_aio_unprepare(s_nout);
    if (ml_aio_destroy) ml_aio_destroy(s_nout);
    s_nout = NULL;
  }
  free(s_ring); s_ring = NULL; s_ring_cap = s_ring_r = s_ring_w = s_ring_fill = 0;
}

/* Try to bring native output up for this stream. Returns 1 when it plays natively. */
static int native_start(int rate, int channels) {
  if (aio_resolve() != 0) return 0;
  if (channels != 1 && channels != 2) {   /* audio-io takes mono/stereo only */
    snprintf(s_aio_reason, sizeof s_aio_reason, "channels-%d", channels);
    return 0;
  }
  ml_audio_out_h out = NULL;
  int rc = ml_aio_create(rate, channels == 1 ? ML_AUDIO_CHANNEL_MONO : ML_AUDIO_CHANNEL_STEREO,
                         ML_AUDIO_SAMPLE_TYPE_S16_LE, &out);
  if (rc != 0 || !out) {
    snprintf(s_aio_reason, sizeof s_aio_reason, "create-%d", rc);
    ALOG("audio_out_create_new(%d Hz, %d ch) failed %d: callback path\n", rate, channels, rc);
    MLT("audio-io create -> %d (fallback)", rc);
    return 0;
  }
  size_t cap = (size_t)rate * (size_t)channels * 2 * (NATIVE_MAX_MS + 50) / 1000;   /* room above the cap */
  size_t pow2 = 4096;
  while (pow2 < cap) pow2 <<= 1;
  unsigned char *ring = malloc(pow2);
  if (!ring) {
    if (ml_aio_destroy) ml_aio_destroy(out);
    snprintf(s_aio_reason, sizeof s_aio_reason, "oom");
    return 0;
  }
  pthread_mutex_lock(&s_nmu);
  s_ring = ring; s_ring_cap = pow2; s_ring_r = s_ring_w = s_ring_fill = 0;
  s_nrate = rate; s_nch = channels;
  s_n_frames = s_n_under = s_n_over = s_n_drops = s_n_write_calls = 0;
  s_nout = out;
  s_native_on = 1;
  pthread_mutex_unlock(&s_nmu);
  if (pthread_create(&s_nthread, NULL, native_worker, NULL) != 0) {
    ALOG("audio worker thread failed: callback path\n");
    MLT("audio-io pthread_create failed (fallback)");
    snprintf(s_aio_reason, sizeof s_aio_reason, "no-thread");
    native_stop();
    return 0;
  }
  s_nthread_up = 1;
  ALOG("native audio output: %d Hz %d ch S16LE, ring %zu B (target %d ms)\n", rate, channels, pow2, NATIVE_TARGET_MS);
  MLT("audio-io native start %d Hz %d ch ring=%zu", rate, channels, pow2);
  return 1;
}

/* decodeAndPlaySample -> one decoded frame into the ring. Never blocks (locks only). */
static void native_feed(const opus_int16 *pcm, int samples) {
  size_t len = (size_t)samples * (size_t)s_nch * 2;
  pthread_mutex_lock(&s_nmu);
  if (!s_native_on) { pthread_mutex_unlock(&s_nmu); return; }
  if (s_ring_fill + len > s_ring_cap) {   /* overfull: drop the oldest data (latency bound) */
    size_t drop = s_ring_fill + len - s_ring_cap;
    size_t keep = s_ring_fill - drop;
    if (keep < s_ring_fill) {   /* advance the read position (mod cap; drop may wrap) */
      s_ring_r = (s_ring_r + drop) & (s_ring_cap - 1);
      s_ring_fill = keep;
    }
    s_n_over++;
  }
  size_t max_ms = (size_t)s_nrate * (size_t)s_nch * 2 * NATIVE_MAX_MS / 1000;
  if (s_ring_fill + len > max_ms) {   /* beyond ~2x target: skip to the newest data */
    size_t want = s_ring_fill + len - max_ms;
    if (want > s_ring_fill) want = s_ring_fill;
    s_ring_r = (s_ring_r + want) & (s_ring_cap - 1);
    s_ring_fill -= want;
    s_n_drops += (unsigned)(want / 1024 + 1);   /* ~frames dropped (a 5 ms stereo frame is 960 B) */
  }
  for (size_t i = 0; i < len; i++) s_ring[(s_ring_w + i) & (s_ring_cap - 1)] = ((const unsigned char *)pcm)[i];
  s_ring_w = (s_ring_w + len) & (s_ring_cap - 1);
  s_ring_fill += len;
  s_n_frames++;
  native_report_locked();   /* a handful of atomic stores: the overlay stays current every frame */
  pthread_cond_signal(&s_ncond);
  pthread_mutex_unlock(&s_nmu);
}

/* 1 while this session plays natively; the engine API exports below (mlengine.h). */
static int native_active(void) { return __atomic_load_n(&s_native_on, __ATOMIC_RELAXED); }

int ml_engine_get_audio_route(void) {
  int r = esplus_stats.aout_route;
  if (r == 0 && native_active()) r = 2;   /* stats not reported yet this tick */
  return r;
}

int ml_engine_get_audio_stats(ml_engine_audio_stats *out) {
  if (!out || out->struct_size < (int32_t)sizeof *out) return GS_INVALID;
  out->route = ml_engine_get_audio_route();
  out->frames = esplus_stats.aout_frames;
  out->underruns = esplus_stats.aout_underruns; out->overruns = esplus_stats.aout_overruns; out->drops = esplus_stats.aout_drops;
  out->buf_ms = esplus_stats.aout_buf_ms;
  return 0;
}

static void ar_cleanup(void) {
#ifdef ML_ENABLE_AC3
  if (s_ac3) { ml_ac3_ready(0); ml_ac3_close(); }
  s_ac3 = 0;
#endif
  native_stop();
  if (s_dec) opus_multistream_decoder_destroy(s_dec);
  free(s_pcm);
  s_dec = NULL; s_pcm = NULL; s_cb = NULL;
  esplus_stats.aout_route = 0;
}

static int ar_init(int audioConfig, const POPUS_MULTISTREAM_CONFIGURATION cfg, void *ctx, int flags) {
  (void)audioConfig; (void)ctx; (void)flags;
  MLT("audio init cfg=0x%x %d Hz %d ch streams=%d", audioConfig, cfg ? cfg->sampleRate : 0,
      cfg ? cfg->channelCount : 0, cfg ? cfg->streams : 0);
  if (!cfg || cfg->channelCount <= 0 || cfg->channelCount > 8 || cfg->sampleRate <= 0 || cfg->samplesPerFrame <= 0) { MLT("audio init -> -1 (config)"); return -1; }
#ifdef ML_ENABLE_AC3
  const int want_ac3 = ml_ac3_waiting();
#endif
  ar_cleanup();   /* a decoder (and native output) left over from a previous session */
  s_samples_per_frame = cfg->samplesPerFrame;
  s_channels = cfg->channelCount;
  s_rate = cfg->sampleRate;
  s_nerr = 0;

  int err = 0;
  s_dec = opus_multistream_decoder_create(cfg->sampleRate, cfg->channelCount, cfg->streams, cfg->coupledStreams, cfg->mapping, &err);
  if (!s_dec) {
    ALOG("opus decoder create failed %d\n", err); MLT("audio init -> -1 (opus %d)", err);
    mldiag_adec(0, err);
    return -1;
  }
  s_pcm = malloc((size_t)s_samples_per_frame * (size_t)s_channels * sizeof *s_pcm);
  if (!s_pcm) { ar_cleanup(); ALOG("out of memory\n"); mldiag_adec(0, OPUS_ALLOC_FAIL); return -1; }

#ifdef ML_ENABLE_AC3
  if (want_ac3) {
    s_ac3 = ml_ac3_open(s_rate, s_channels) && esplus_enable_ac3();
    if (!s_ac3) { ALOG("AC-3 unavailable, falling back to the PCM callback\n"); ml_ac3_close(); }
    esplus_prepare();
    ml_ac3_ready(s_ac3);
    if (s_ac3) {
      mldiag_aout_ac3();
      esplus_stats.aout_route = 2;  /* native playback, no host/UI PCM bridge */
      return 0;
    }
  }
#endif
  s_cb = __atomic_load_n(&s_cb_set, __ATOMIC_ACQUIRE);
  int native = native_start(cfg->sampleRate, cfg->channelCount);
  if (native) {
    mldiag_aout_native(1);
    pthread_mutex_lock(&s_nmu);
    native_report_locked();
    pthread_mutex_unlock(&s_nmu);
  } else {
    esplus_stats.aout_route = s_cb ? 1 : 0;
    mldiag_aout(s_cb != NULL);              /* which output this session uses (may be none) */
    if (s_cb) mldiag_aout_native(0);        /* ...after a failed native attempt: the fallback variant of the section */
#ifdef ML_ENABLE_AC3
    mldiag_aout_reason(want_ac3 ? "ac3-unavailable" : s_aio_reason);
#else
    mldiag_aout_reason(s_aio_reason);
#endif
    ALOG("audio output: callback path (%s; native reason: %s)\n", s_cb ? "set" : "missing", s_aio_reason);
    if (!s_cb) { ALOG("audio output unavailable (no callback)\n"); MLT("audio output unavailable (no callback)"); }
    else { ALOG("init %d Hz, %d ch, %d samples/frame, PCM S16LE -> callback path\n", cfg->sampleRate, cfg->channelCount, cfg->samplesPerFrame); }
  }
  if (native) {
    ALOG("init %d Hz, %d ch, %d samples/frame, PCM S16LE -> native audio-io\n", cfg->sampleRate, cfg->channelCount, cfg->samplesPerFrame);
  }
  MLT("audio init %d Hz %d ch spf=%d native=%d cb=%s", cfg->sampleRate, cfg->channelCount, cfg->samplesPerFrame, native, s_cb ? "set" : "missing");
  return 0;
}

static void ar_start(void) {}
static void ar_stop(void) {}

static void ar_decodePlay(char *data, int len) {
  if (!s_dec || !s_pcm) return;
  int native = __atomic_load_n(&s_native_on, __ATOMIC_RELAXED);
#ifdef ML_ENABLE_AC3
  if (!native && !s_cb && !s_ac3) return;
#else
  if (!native && !s_cb) return;   /* nothing to feed: skip the decode */
#endif
  /* no data = a lost packet: libopus conceals it */
  int n = (data == NULL || len <= 0)
      ? opus_multistream_decode(s_dec, NULL, 0, s_pcm, s_samples_per_frame, 0)
      : opus_multistream_decode(s_dec, (const unsigned char *)data, len, s_pcm, s_samples_per_frame, 0);
  if (n <= 0) {
    mldiag_adec(0, n);
    if (s_nerr++ < 4 || s_nerr % 256 == 0) MLT("opus decode error %d (#%u)", n, s_nerr);
    return;   /* nothing is handed over: the output fills the gap */
  }
  mldiag_adec(1, 0);
#ifdef ML_ENABLE_AC3
  if (s_ac3) { ml_ac3_feed(s_pcm, n); return; }
#endif
  if (native) {
    native_feed(s_pcm, n);
    return;
  }
  if (!s_cb) return;
  mldiag_aframe(n * s_channels * 2);
  s_cb((const short *)s_pcm, n, s_channels, s_rate);   /* the host copies and returns at once */
}

void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb) {
  LiInitializeAudioCallbacks(cb);
  cb->init = ar_init;
  cb->start = ar_start;
  cb->stop = ar_stop;
  cb->cleanup = ar_cleanup;
  cb->decodeAndPlaySample = ar_decodePlay;
  /* DIRECT_SUBMIT: decodeAndPlaySample runs on the receive thread. Without it common-c's 30-packet audio queue
     drops ALL queued packets on overflow (a 150 ms gap). A fixed-point decode of a 5 ms frame is cheap enough for
     that thread, and the output paths only copy the frame (native: into the ring; callback: the host copies). */
  cb->capabilities = CAPABILITY_DIRECT_SUBMIT;
}

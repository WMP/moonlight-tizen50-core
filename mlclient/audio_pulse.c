/* audio_pulse.c - see audio_pulse.h. Host self-test: tests/audio_pulse_test.c. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "audio_pulse.h"
#include "mldiag.h"
#include "mltrace.h"

/* libpulse types, declared here (same layout as pulse/sample.h, def.h, simple.h) */
typedef struct { int format; uint32_t rate; uint8_t channels; } pa_sample_spec;
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } pa_buffer_attr;
#define PA_SAMPLE_S16LE 3
#define PA_STREAM_PLAYBACK 1
#define PA_DEFAULT ((uint32_t)-1)

/* pa_channel_map (pulse/channelmap.h): PA_CHANNELS_MAX is 32, pa_channel_position_t is an int enum */
typedef struct { uint8_t channels; int map[32]; } pa_channel_map;

typedef void *(*pa_simple_new_t)(const char *server, const char *name, int dir, const char *dev, const char *stream_name,
                                 const pa_sample_spec *ss, const pa_channel_map *map, const pa_buffer_attr *attr, int *error);
typedef int (*pa_simple_write_t)(void *s, const void *data, size_t bytes, int *error);
typedef uint64_t (*pa_simple_get_latency_t)(void *s, int *error);
typedef void (*pa_simple_free_t)(void *s);
typedef const char *(*pa_strerror_t)(int error);

static struct {
  pa_simple_new_t snew; pa_simple_write_t write;
  pa_simple_get_latency_t latency; pa_simple_free_t free; pa_strerror_t strerror;   /* strerror: optional, trace only */
} s_pa;
/* Used from common-c's audio thread (write) and from init/cleanup (open/close), which common-c calls while that thread
   is not running: no lock. The other threads only read the values mldiag keeps (atomic there). */
static void *s_stream;
static int s_rate, s_channels, s_lat_ms, s_last_err, s_native, s_want;   /* s_want: opened once, so a write may reopen */
static unsigned s_wfail, s_reopen;
static uint64_t s_samples;      /* per channel, written this session */
static uint64_t s_lat_t;        /* monotonic ms of the last latency query */
static uint64_t s_lat_us;
static uint64_t s_fail_t;       /* monotonic ms of the last failed write or reopen */
#define REOPEN_MS 2000

static uint64_t mono_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

__attribute__((unused)) static const char *perr(int e) { return s_pa.strerror ? s_pa.strerror(e) : "?"; }

/* 0 = loaded; else the mldiag reason (1 dlopen, 2 dlsym) */
static int load_lib(void) {
  memset(&s_pa, 0, sizeof s_pa);   /* loaded at each open: dlopen is refcounted, and a failed try leaves nothing behind */
  void *h = dlopen("libpulse-simple.so.0", RTLD_NOW);
  if (!h) h = dlopen("libpulse-simple.so", RTLD_NOW);
  if (!h) { MLT("audio pulse: dlopen failed"); return 1; }
  s_pa.snew = (pa_simple_new_t)dlsym(h, "pa_simple_new");
  s_pa.write = (pa_simple_write_t)dlsym(h, "pa_simple_write");
  s_pa.latency = (pa_simple_get_latency_t)dlsym(h, "pa_simple_get_latency");
  s_pa.free = (pa_simple_free_t)dlsym(h, "pa_simple_free");
  s_pa.strerror = (pa_strerror_t)dlsym(h, "pa_strerror");   /* may be in libpulse.so: optional */
  if (!s_pa.snew || !s_pa.write || !s_pa.latency || !s_pa.free) {
    MLT("audio pulse: missing symbol"); return 2;   /* the handle stays open: harmless */
  }
  return 0;
}

static void publish(void) {
  mldiag_aout_state((unsigned)s_lat_us, s_last_err, (uint32_t)(s_samples * 1000 / (uint64_t)(s_rate > 0 ? s_rate : 1)), s_wfail, s_reopen);
}

/* The channel order of the decoded PCM is the Moonlight one (moonlight-common-c RtspConnection.c builds the Opus
   mapping so that the output is FL FR C LFE RL RR [SL SR]; for 5.1 and 7.1 it moves the LFE after C). PulseAudio's
   default 5.1 map is FL FR RL RR C LFE and it has none for 7 or 8 channels, so the map is explicit.
   Positions (pulse/channelmap.h): MONO 0, FL 1, FR 2, FC 3, RL 5, RR 6, LFE 7, SL 10, SR 11.
   Returns NULL for other counts: the PulseAudio default. */
static const pa_channel_map *channel_map(int channels, pa_channel_map *m) {
  static const int mono[] = { 0 }, stereo[] = { 1, 2 }, s51[] = { 1, 2, 3, 7, 5, 6 }, s71[] = { 1, 2, 3, 7, 5, 6, 10, 11 };
  const int *p = channels == 1 ? mono : channels == 2 ? stereo : channels == 6 ? s51 : channels == 8 ? s71 : NULL;
  if (!p) return NULL;
  memset(m, 0, sizeof *m);
  m->channels = (uint8_t)channels;
  for (int i = 0; i < channels; i++) m->map[i] = p[i];
  return m;
}

/* one stream: the default server, then the PulseAudio socket of the TV. 0 = ok, else the PA error. */
static int new_stream(void) {
  pa_sample_spec ss = { PA_SAMPLE_S16LE, (uint32_t)s_rate, (uint8_t)s_channels };
  pa_buffer_attr at = { PA_DEFAULT, PA_DEFAULT, 0, PA_DEFAULT, PA_DEFAULT };
  pa_channel_map cm;
  const pa_channel_map *map = channel_map(s_channels, &cm);
  if (s_lat_ms > 0) at.tlength = (uint32_t)((uint64_t)s_lat_ms * (uint64_t)s_rate * (uint64_t)s_channels * 2 / 1000);
  int err = 0;
  s_native = 0;
  s_stream = s_pa.snew(NULL, "moonlight", PA_STREAM_PLAYBACK, NULL, "game audio", &ss, map, &at, &err);
  if (!s_stream && !getenv("PULSE_SERVER")) {
    MLT("audio pulse: default server failed (%d %s), trying unix:/run/pulse/native", err, perr(err));
    int err2 = 0;
    s_stream = s_pa.snew("unix:/run/pulse/native", "moonlight", PA_STREAM_PLAYBACK, NULL, "game audio", &ss, map, &at, &err2);
    if (s_stream) s_native = 1; else err = err2;
  }
  if (!s_stream) { s_last_err = err; MLT("audio pulse: pa_simple_new failed %d %s", err, perr(err)); return err ? err : -1; }
  MLT("audio pulse: open %d Hz %d ch tlength=%u server=%s", s_rate, s_channels, at.tlength, s_native ? "native" : "default");
  return 0;
}

int audio_pulse_open(int rate, int channels, int latency_ms) {
  audio_pulse_close();
  s_last_err = 0; s_samples = 0; s_lat_t = 0; s_lat_us = 0; s_wfail = s_reopen = 0; s_fail_t = 0;
  if (rate <= 0 || channels <= 0 || channels > 255) { mldiag_aout_none(3, 0); return -1; }
  int why = load_lib();
  if (why) { mldiag_aout_none(why, 0); return -1; }
  s_rate = rate; s_channels = channels; s_lat_ms = latency_ms;
  int err = new_stream();
  if (err) { mldiag_aout_none(3, err); return -1; }
  s_want = 1;
  mldiag_aout_open(s_native);
  return 0;
}

int audio_pulse_write(const void *pcm, size_t bytes) {
  if (!s_stream) {
    /* after a failed write: one reopen per 2 s, on a write */
    if (!s_want) return -1;
    uint64_t now = mono_ms();
    if (now - s_fail_t < REOPEN_MS) return -1;
    s_reopen++;
    if (new_stream() != 0) { s_fail_t = now; publish(); return -1; }
    MLT("audio pulse: reopened (#%u)", s_reopen);
    s_lat_t = 0; publish();
  }
  int err = 0;
  if (s_pa.write(s_stream, pcm, bytes, &err) < 0) {
    s_last_err = err;
    if (s_wfail++ < 4 || s_wfail % 256 == 0) MLT("audio pulse: write failed %d %s (#%u)", err, perr(err), s_wfail);
    s_pa.free(s_stream); s_stream = NULL; s_fail_t = mono_ms();
    publish();
    return -1;
  }
  s_samples += bytes / ((size_t)s_channels * 2);
  uint64_t now = mono_ms();
  if (!s_lat_t || now - s_lat_t >= 1000) {   /* about once per second */
    s_lat_t = now;
    int lerr = 0;
    uint64_t us = s_pa.latency(s_stream, &lerr);
    if (us != (uint64_t)-1) s_lat_us = us; else s_last_err = lerr;   /* -1 = error */
    publish();
    MLT("audio pulse: latency %llu us, written %llu ms", (unsigned long long)s_lat_us, (unsigned long long)(s_samples * 1000 / (uint64_t)s_rate));
  }
  return 0;
}

void audio_pulse_close(void) {
  s_want = 0;
  if (!s_stream) return;
  publish();
  s_pa.free(s_stream);
  s_stream = NULL;
}

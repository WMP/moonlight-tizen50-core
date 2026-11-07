/*
 * Host self-test for mlclient/audio_pulse.c. No real libpulse: dlopen/dlsym are wrapped
 * (-Wl,--wrap=dlopen,--wrap=dlsym) and the clock is wrapped (--wrap=clock_gettime) and hand out stub pa_* functions.
 * Run: sh tests/build-esplus-ecore-test.sh
 */
#include <assert.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "audio_pulse.h"
#include "mldiag.h"


typedef struct { int format; uint32_t rate; uint8_t channels; } ss_t;
typedef struct { uint8_t channels; int map[32]; } map_t;
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } attr_t;

static int dl_ok = 1, dl_first_fails, miss_sym, new_fails;   /* new_fails: 1 = NULL server fails, 2 = every server fails */
static int dlopen_calls, new_calls, write_calls, free_calls, write_rc, strerror_hidden;
static char dlopen_names[2][32];
static const char *servers[4];
static map_t last_map; static int last_map_null;
static ss_t last_ss; static attr_t last_attr; static size_t wrote;
static uint64_t latency = 41000; static int latency_calls;

static void *st_new(const char *server, const char *name, int dir, const char *dev, const char *sn, const ss_t *ss, const map_t *map, const attr_t *at, int *err) {
  (void)name; (void)dev; (void)sn;
  last_map_null = !map; if (map) last_map = *map;
  assert(dir == 1);
  servers[new_calls < 4 ? new_calls : 3] = server; new_calls++;
  last_ss = *ss; last_attr = *at;
  if (new_fails == 2 || (new_fails == 1 && !server)) { *err = 6; return NULL; }
  return (void *)&last_ss;
}
static int st_write(void *s, const void *d, size_t n, int *err) { (void)s; (void)d; write_calls++; if (write_rc) { *err = 9; return -1; } wrote += n; return 0; }
static uint64_t st_lat(void *s, int *err) { (void)s; (void)err; latency_calls++; return latency; }
static void st_free(void *s) { (void)s; free_calls++; }
static const char *st_strerror(int e) { (void)e; return "stub error"; }

/* fake clock: fake_ms < 0 = the real one (the MLT_TRACE build and mldiag use it too) */
static long long fake_ms = -1;
int __real_clock_gettime(clockid_t c, struct timespec *ts);
int __wrap_clock_gettime(clockid_t c, struct timespec *ts) {
  if (fake_ms < 0) return __real_clock_gettime(c, ts);
  ts->tv_sec = fake_ms / 1000; ts->tv_nsec = (fake_ms % 1000) * 1000000; return 0;
}

/* the MLT_TRACE build also calls dlopen/dlsym (dlog lookup): those go to the real ones */
void *__real_dlopen(const char *f, int flags);
void *__real_dlsym(void *h, const char *n);
void *__wrap_dlopen(const char *f, int flags) {
  if (strncmp(f, "libpulse-simple", 15)) return __real_dlopen(f, flags);
  if (dlopen_calls < 2) snprintf(dlopen_names[dlopen_calls], sizeof dlopen_names[0], "%s", f);
  dlopen_calls++;
  if (!dl_ok || (dl_first_fails && dlopen_calls % 2 == 1)) return NULL;
  return (void *)&dl_ok;
}
void *__wrap_dlsym(void *h, const char *n) {
  if (h != (void *)&dl_ok) return __real_dlsym(h, n);
  if (miss_sym && !strcmp(n, "pa_simple_free")) return NULL;
  if (!strcmp(n, "pa_simple_new")) return (void *)st_new;
  if (!strcmp(n, "pa_simple_write")) return (void *)st_write;
  if (!strcmp(n, "pa_simple_get_latency")) return (void *)st_lat;
  if (!strcmp(n, "pa_simple_free")) return (void *)st_free;
  if (!strcmp(n, "pa_strerror")) return strerror_hidden ? NULL : (void *)st_strerror;
  return NULL;
}

static char line[4096];
static const char *diag(void) { mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line); return line; }

int main(void) {
  unsigned char pcm[960] = {0};
  unsetenv("PULSE_SERVER");
  mldiag_reset(NULL);

  /* ok: S16LE 48000 stereo, tlength for 40 ms = 40*48000*2*2/1000 = 7680 bytes, prebuf 0, the rest default */
  assert(audio_pulse_open(48000, 2, 40) == 0);
  assert(strcmp(dlopen_names[0], "libpulse-simple.so.0") == 0 && dlopen_calls == 1);
  assert(last_ss.format == 3 && last_ss.rate == 48000 && last_ss.channels == 2);
  assert(last_attr.tlength == 7680 && last_attr.prebuf == 0);
  assert(last_attr.maxlength == (uint32_t)-1 && last_attr.minreq == (uint32_t)-1 && last_attr.fragsize == (uint32_t)-1);
  assert(new_calls == 1 && servers[0] == NULL);
  assert(strstr(diag(), " | aout pulse server=default lat=0 err=0 written_ms=0 wfail=0 reopen=0 | "));

  /* write: 960 bytes = 240 samples = 5 ms; the first write queries the latency; written_ms counts per sample */
  assert(audio_pulse_write(pcm, sizeof pcm) == 0);
  assert(write_calls == 1 && wrote == 960 && latency_calls == 1);
  for (int i = 0; i < 9; i++) assert(audio_pulse_write(pcm, sizeof pcm) == 0);
  assert(latency_calls == 1);
  assert(strstr(diag(), " | aout pulse server=default lat=41000 err=0 written_ms=5 wfail=0 reopen=0 | "));   /* the diagnostics refresh with the latency query */
  /* the channel map: explicit for 1, 2, 6, 8 channels (Moonlight order), NULL otherwise */
  assert(!last_map_null && last_map.channels == 2 && last_map.map[0] == 1 && last_map.map[1] == 2);
  audio_pulse_close();
  { static const int m51[] = { 1, 2, 3, 7, 5, 6 }, m71[] = { 1, 2, 3, 7, 5, 6, 10, 11 };
    assert(audio_pulse_open(48000, 1, 0) == 0 && !last_map_null && last_map.channels == 1 && last_map.map[0] == 0); audio_pulse_close();
    assert(audio_pulse_open(48000, 6, 0) == 0 && !last_map_null && last_map.channels == 6 && !memcmp(last_map.map, m51, sizeof m51)); audio_pulse_close();
    assert(audio_pulse_open(48000, 8, 0) == 0 && !last_map_null && last_map.channels == 8 && !memcmp(last_map.map, m71, sizeof m71)); audio_pulse_close();
    assert(audio_pulse_open(48000, 4, 0) == 0 && last_map_null); audio_pulse_close(); }
  assert(audio_pulse_open(48000, 2, 40) == 0);
  free_calls = 0; write_calls = 0; new_calls = 0;

  /* a failed write: counted, the stream is closed, no new try for 2 s; then one reopen on the next write */
  fake_ms = 100000;
  write_rc = 1; assert(audio_pulse_write(pcm, sizeof pcm) == -1);
  assert(write_calls == 1 && free_calls == 1 && strstr(diag(), "err=9 written_ms=0 wfail=1 reopen=0 | "));
  fake_ms = 101999; assert(audio_pulse_write(pcm, sizeof pcm) == -1 && write_calls == 1 && new_calls == 0);
  new_fails = 2;
  fake_ms = 102000; assert(audio_pulse_write(pcm, sizeof pcm) == -1 && new_calls == 2 && write_calls == 1);   /* reopen fails: the timer restarts */
  assert(strstr(diag(), " wfail=1 reopen=1 | "));
  new_fails = 0;
  fake_ms = 103999; assert(audio_pulse_write(pcm, sizeof pcm) == -1 && new_calls == 2);
  fake_ms = 104000; write_rc = 0; assert(audio_pulse_write(pcm, sizeof pcm) == 0 && new_calls == 3 && write_calls == 2);
  assert(strstr(diag(), " wfail=1 reopen=2 | "));
  audio_pulse_close(); assert(free_calls == 2);
  audio_pulse_close(); assert(free_calls == 2);   /* closed: no-op */
  fake_ms = 200000; assert(audio_pulse_write(pcm, sizeof pcm) == -1 && write_calls == 2 && new_calls == 3);   /* closed on purpose: no reopen */
  fake_ms = -1;

  /* the default server fails: the native socket is tried; a second open starts the counters again */
  new_calls = 0; new_fails = 1;
  assert(audio_pulse_open(48000, 2, 40) == 0);
  assert(new_calls == 2 && servers[0] == NULL && strcmp(servers[1], "unix:/run/pulse/native") == 0);
  assert(strstr(diag(), " | aout pulse server=native "));
  free_calls = 0;
  assert(audio_pulse_open(48000, 2, 0) == 0);   /* open twice: the old stream is closed; 0 = default tlength */
  assert(last_attr.tlength == (uint32_t)-1 && free_calls == 1);
  audio_pulse_close();

  /* PULSE_SERVER set: no second try */
  setenv("PULSE_SERVER", "unix:/x", 1); new_calls = 0;
  assert(audio_pulse_open(48000, 2, 40) == -1 && new_calls == 1);
  unsetenv("PULSE_SERVER");
  /* every server fails: reason new + the PA error */
  new_calls = 0; new_fails = 2;
  assert(audio_pulse_open(48000, 2, 40) == -1 && new_calls == 2);
  assert(strstr(diag(), " | aout none reason=new err=6 | "));
  assert(audio_pulse_write(pcm, sizeof pcm) == -1);
  new_fails = 0;

  /* other rates, channels and latencies: 8 ch, 44100 Hz, 10 ms = 10*44100*8*2/1000 = 7056 */
  assert(audio_pulse_open(44100, 8, 10) == 0 && last_ss.rate == 44100 && last_ss.channels == 8 && last_attr.tlength == 7056);
  audio_pulse_close();

  /* a missing required symbol: open fails cleanly (and the pa_strerror stays optional) */
  miss_sym = 1; strerror_hidden = 1;
  assert(audio_pulse_open(48000, 2, 40) == -1);
  assert(strstr(diag(), " | aout none reason=dlsym err=0 | "));
  assert(audio_pulse_write(pcm, sizeof pcm) == -1);
  miss_sym = 0;

  /* no library at all; the second name is tried first */
  dl_ok = 0; dlopen_calls = 0;
  assert(audio_pulse_open(48000, 2, 40) == -1 && dlopen_calls == 2);
  assert(strcmp(dlopen_names[1], "libpulse-simple.so") == 0);
  assert(strstr(diag(), " | aout none reason=dlopen err=0 | "));
  dl_ok = 1; dl_first_fails = 1; dlopen_calls = 0;   /* ".so.0" missing, ".so" found */
  assert(audio_pulse_open(48000, 2, 40) == 0 && dlopen_calls == 2);
  audio_pulse_close();

  /* bad arguments */
  assert(audio_pulse_open(0, 2, 40) == -1 && audio_pulse_open(48000, 0, 40) == -1);
  puts("audio_pulse_test OK");
  return 0;
}

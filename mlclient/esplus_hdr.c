/*
 * esplus_hdr.c - passes HDR10 colour info to the TV video pipeline.
 *
 * libtrackrenderer.so exports
 *   int trackrenderer_set_matroska_color_info(void *tr, const char *color_info)   0 = ok, -1 = error
 *   int trackrenderer_get_playing_time(void *tr, uint64_t *time)                  0 = ok, -1 = error
 *   int trackrenderer_get_state(void *tr)                                         optional
 * The trackrenderer handle is found through the esplusplayer handle of the TV 5.0 player:
 *   adapter = *(void **)(h + 0x1B4);  tr = *(void **)(adapter + 0);
 * Every pointer is probed for readability first, and the chain is checked by comparing the playing
 * times of both objects. A failure is returned, never fatal.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esplus_hdr.h"
#include "mltrace.h"

typedef int (*tr_set_fn)(void *tr, const char *text);
typedef int (*tr_time_fn)(void *tr, uint64_t *time);
typedef int (*tr_state_fn)(void *tr);

#define ADAPTER_OFF 0x1B4   /* adapter pointer inside the TV 5.0 player object */

static void *s_lib;
static tr_set_fn s_set;
static tr_time_fn s_time;
static tr_state_fn s_state;
static int s_tried;
static int s_pipe[2] = {-1, -1};

size_t hdr_color_text(char *out, size_t n, const uint16_t p[6], uint16_t wx, uint16_t wy,
                      unsigned max_lum, unsigned min_lum, unsigned cll, unsigned fall, int full_range) {
  int k = snprintf(out, n,
      "matrixCoefficients:9 bitsPerChannel:10 chromaSubsamplingHorz:1 chromaSubsamplingVert:1 cbSubsamplingHorz:0 cbSubsamplingVert:0 "
      "chromaSitingHorz:1 chromaSitingVert:2 range:%u transferCharacteristics:16 primaries:9 maxCLL:%u maxFALL:%u "
      "RX:%.5f RY:%.5f GX:%.5f GY:%.5f BX:%.5f BY:%.5f wX:%.5f wY:%.5f luminanceMax:%.4f luminanceMin:%.4f isHDR10p:0",
      full_range ? 2u : 1u, cll, fall,
      p[0] / 50000.0, p[1] / 50000.0, p[2] / 50000.0, p[3] / 50000.0, p[4] / 50000.0, p[5] / 50000.0,
      wx / 50000.0, wy / 50000.0, (double)max_lum, min_lum / 10000.0);
  return k > 0 && (size_t)k < n ? (size_t)k : 0;
}

void esplus_hdr_load(void) {
  if (s_tried) return;
  s_tried = 1;
  s_lib = dlopen("libtrackrenderer.so", 2 /* RTLD_NOW */);
  if (s_lib) {
    s_set = (tr_set_fn)dlsym(s_lib, "trackrenderer_set_matroska_color_info");
    s_time = (tr_time_fn)dlsym(s_lib, "trackrenderer_get_playing_time");
    s_state = (tr_state_fn)dlsym(s_lib, "trackrenderer_get_state");   /* optional */
  }
  MLT("hdr: trackrenderer lib=%d sym=%d%d%d", s_lib != NULL, s_set != NULL, s_time != NULL, s_state != NULL);
}

/* 1 when a pointer-sized value at p can be read: write() to a pipe fails with EFAULT for unreadable memory */
static int readable(const void *p) {
  char b[sizeof(void *)];
  if (s_pipe[0] < 0 && pipe(s_pipe)) { s_pipe[0] = s_pipe[1] = -1; return 0; }
  if (write(s_pipe[1], p, sizeof b) != (ssize_t)sizeof b) return 0;
  return read(s_pipe[0], b, sizeof b) == (ssize_t)sizeof b;
}

static void *load_ptr(const void *p) { void *v; memcpy(&v, p, sizeof v); return v; }

static const char *word(char c) { return c == 'o' ? "ok" : c == 'n' ? "null" : c == 'u' ? "unreadable" : "-"; }

static int finish(int rc, const char *txt, esplus_hdr_diag *d) {
  d->steps[0] = d->adapter; d->steps[1] = d->handle; d->steps[2] = d->steps[2] ? d->steps[2] : '-'; d->steps[3] = 0;
  (void)txt;   /* only the trace prints it */
  char line[256];
  snprintf(line, sizeof line, "hdr color: rc=%d lib=%d sym=%d%d%d adapter=%s handle=%s tr_time=%d/%llu pl_time=%d/%llu state=%d set=%d steps=%s",
           rc, d->lib, d->sym_set, d->sym_time, d->sym_state, word(d->adapter), word(d->handle), d->tr_rc, d->tr_ms, d->pl_rc, d->pl_ms,
           d->state, d->set_rc, d->steps);
  fprintf(stderr, "[esplus] %s\n", line);
  MLT("%s", line);   /* the console sees only the trace */
  MLT("hdr color text: %s", txt);
  return rc;
}

int esplus_hdr_apply(esplusplayer_handle h, const char *txt, esplus_hdr_diag *d) {
  memset(d, 0, sizeof *d);
  d->adapter = d->handle = '-'; d->tr_rc = d->pl_rc = d->state = d->set_rc = -99;
  esplus_hdr_load();
  d->lib = s_lib != NULL; d->sym_set = s_set != NULL; d->sym_time = s_time != NULL; d->sym_state = s_state != NULL;
  if (!s_set || !s_time) return finish(-1, txt, d);
  if (!h) { d->adapter = 'n'; return finish(-2, txt, d); }
  const char *slot = (const char *)h + ADAPTER_OFF;
  if (!readable(slot)) { d->adapter = 'u'; return finish(-2, txt, d); }
  void *adapter = load_ptr(slot);
  if (!adapter) { d->adapter = 'n'; return finish(-2, txt, d); }
  d->adapter = 'o';
  if (!readable(adapter)) { d->handle = 'u'; return finish(-2, txt, d); }
  void *tr = load_ptr(adapter);
  if (!tr) { d->handle = 'n'; return finish(-2, txt, d); }
  d->handle = 'o';
  uint64_t a = 0, b = 0;
  d->tr_rc = s_time(tr, &a); d->tr_ms = a;
  d->pl_rc = esplusplayer_get_playing_time(h, &b) ? 0 : -1; d->pl_ms = b;   /* the player call returns non-zero on success */
  if (d->tr_rc != 0 || d->pl_rc != 0 || (a > b ? a - b : b - a) >= 100) { d->steps[2] = 'm'; return finish(-3, txt, d); }
  d->steps[2] = 'o';
  if (s_state) d->state = s_state(tr);
  d->set_rc = s_set(tr, txt);
  return finish(d->set_rc == 0 ? 1 : -4, txt, d);
}

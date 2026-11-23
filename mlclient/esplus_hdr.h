/*
 * esplus_hdr.h - HDR10 colour info for the TV video pipeline (libtrackrenderer.so).
 * Not part of the ABI (mlengine.h).
 */
#ifndef ESPLUS_HDR_H
#define ESPLUS_HDR_H

#include <stddef.h>
#include <stdint.h>

#include "esplusplayer_tv50.h"

/* Builds the one-line colour text of trackrenderer_set_matroska_color_info (HDR10, BT.2020, PQ).
   prim_rgb_xy: R, G, B primaries as x,y in 1/50000; wp_*: white point in 1/50000; max_lum_nits; min_lum_1e4: 1/10000 nit.
   Returns the text length, or 0 when it does not fit in n bytes. */
size_t hdr_color_text(char *out, size_t n, const uint16_t prim_rgb_xy[6], uint16_t wp_x, uint16_t wp_y,
                      unsigned max_lum_nits, unsigned min_lum_1e4, unsigned max_cll, unsigned max_fall, int full_range);

/* dlopen libtrackrenderer.so and resolve its symbols once (cached). Safe to call from any thread before s_lock paths. */
void esplus_hdr_load(void);

/* What each step of esplus_hdr_apply found. Letters: o ok, n NULL, u unreadable, m mismatch, - not reached.
   tr_/pl_ rc and ms: result and value of the playing-time calls (-99 = not called); state -99 = not read; set_rc -99 = not called. */
typedef struct {
  int lib, sym_set, sym_time, sym_state;   /* 1 = found */
  char adapter, handle;                    /* adapter: h+0x1B4; handle: adapter+0 (the trackrenderer) */
  int tr_rc, pl_rc, state, set_rc;
  unsigned long long tr_ms, pl_ms;
  char steps[4];                           /* adapter, handle, time: the three letters above (NUL-terminated) */
} esplus_hdr_diag;

/* Gives the text to the trackrenderer of player h and fills *d (never NULL). Call after prepare_async_done and before stop.
   Writes one stderr line with all steps. Pointers are probed for readability before each read: it must not crash.
   1 ok, -1 library or required symbol missing, -2 pointer NULL or unreadable, -3 chain check failed, -4 set call failed. */
int esplus_hdr_apply(esplusplayer_handle h, const char *txt, esplus_hdr_diag *d);

#endif

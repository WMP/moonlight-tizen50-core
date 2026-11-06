/*
 * Host self-test for the video sink in mlclient/video_esplus.c (VidDecSubmit/VidDecStart),
 * driven through VidEsplus_FillCallbacks. EFL and esplusplayer are stubbed; the submit
 * stub records every packet and returns a scripted status sequence.
 * Run: sh tests/build-esplus-ecore-test.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "Limelight.h"
#include "esplus_stats.h"
#include "esplusplayer_tv50.h"
int ecore_cnt, wl2_cnt, disp_ok = 1, win_ok = 1, calls_off_main;
int ecore_init(void) { return ++ecore_cnt; }
int ecore_shutdown(void) { return --ecore_cnt; }
int ecore_wl2_init(void) { return ++wl2_cnt; }
int ecore_wl2_shutdown(void) { return --wl2_cnt; }
static int d, w;
void *ecore_wl2_display_connect(const char *n) { (void)n; return disp_ok ? &d : NULL; }
void ecore_wl2_display_disconnect(void *x) { (void)x; }
void *ecore_wl2_window_new(void *a, void *b, int x, int y, int ww, int hh) { (void)a;(void)b;(void)x;(void)y;(void)ww;(void)hh; return win_ok ? &w : NULL; }
void ecore_wl2_window_show(void *x) { (void)x; }
void ecore_wl2_window_alpha_set(void *x, unsigned char a) { (void)x;(void)a; }
void ecore_wl2_window_free(void *x) { (void)x; }
void ecore_wl2_display_flush(void *x) { (void)x; }
void *ecore_main_loop_thread_safe_call_sync(void *(*cb)(void *), void *data) { return cb(data); }
static int pl;
esplusplayer_handle esplusplayer_create(void) { return (esplusplayer_handle)&pl; }
int esplusplayer_open(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_close(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_stop(esplusplayer_handle h) { (void)h; return 1; }
static esplusplayer_prepare_async_done_cb cb_prepared;
static esplusplayer_error_cb cb_error;
static esplusplayer_buffer_status_cb cb_buffer;
static esplusplayer_resource_conflicted_cb cb_conflict;
void esplusplayer_set_error_cb(esplusplayer_handle h, esplusplayer_error_cb c) { (void)h; cb_error = c; }
void esplusplayer_set_buffer_status_cb(esplusplayer_handle h, esplusplayer_buffer_status_cb c) { (void)h; cb_buffer = c; }
void esplusplayer_set_resource_conflicted_cb(esplusplayer_handle h, esplusplayer_resource_conflicted_cb c) { (void)h; cb_conflict = c; }
void esplusplayer_set_ready_to_prepare_cb(esplusplayer_handle h, esplusplayer_ready_to_prepare_cb c) { (void)h;(void)c; }
void esplusplayer_set_prepare_async_done_cb(esplusplayer_handle h, esplusplayer_prepare_async_done_cb c) { (void)h; cb_prepared = c; }
int esplusplayer_prepare_async(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_start(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_get_state(esplusplayer_handle h) { (void)h; return 2; }
int esplusplayer_set_ecore_display(esplusplayer_handle h, esplusplayer_display_type t, void *win, int x, int y, int ww, int hh) { (void)h;(void)t;(void)win;(void)x;(void)y;(void)ww;(void)hh; return 1; }
int esplusplayer_set_display_mode(esplusplayer_handle h, esplusplayer_display_mode m) { (void)h;(void)m; return 1; }
static int seek_calls; static uint64_t seek_target;
int esplusplayer_seek(esplusplayer_handle h, uint64_t t) { (void)h; seek_calls++; seek_target = t; return 1; }
void esplusplayer_set_seek_done_cb(esplusplayer_handle h, esplusplayer_seek_done_cb c) { (void)h;(void)c; }
int esplusplayer_add_video_stream_info(esplusplayer_handle h, const esplusplayer_video_stream_info *i) { (void)h;(void)i; return 1; }
static uint64_t play_ms = 1;
int esplusplayer_get_playing_time(esplusplayer_handle h, uint64_t *t) { (void)h; *t = play_ms; return 1; }
/* submit stub: record packets; return script[0..n_script) in order, then SUCCESS */
#define MAXP 64
static int sub_calls, script[16], n_script; static uint64_t rec_ms[MAXP];
int esplusplayer_submit_packet(esplusplayer_handle h, const esplusplayer_es_packet *p) {
  (void)h; int r = sub_calls < n_script ? script[sub_calls] : ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS;
  assert(p->type == ESPLUSPLAYER_STREAM_TYPE_VIDEO && sub_calls < MAXP);
  rec_ms[sub_calls++] = p->pts / 1000000; return r;
}
static int idr_calls;
void LiRequestIdrFrame(void) { idr_calls++; }
void LiInitializeVideoCallbacks(PDECODER_RENDERER_CALLBACKS c) { memset(c, 0, sizeof *c); }
extern void VidEsplus_FillCallbacks(PDECODER_RENDERER_CALLBACKS cb);

static DECODER_RENDERER_CALLBACKS cb;
static void script_set(int n, int st) { n_script = n; for (int i = 0; i < n; i++) script[i] = st; }
/* fresh session: mode may be NULL (off); depth thread off; player prepared */
static void begin(const char *mode, const char *drop) {
  sub_calls = 0; n_script = 0; idr_calls = 0; play_ms = 1;
  setenv("MLENGINE_DEPTH_POLL_MS", "0", 1);
  if (mode) setenv("MLENGINE_LATENCY_MODE", mode, 1); else unsetenv("MLENGINE_LATENCY_MODE");
  if (drop) setenv("MLENGINE_DROP_AFTER_MS", drop, 1); else unsetenv("MLENGINE_DROP_AFTER_MS");
  VidEsplus_FillCallbacks(&cb);
  assert(cb.setup(VIDEO_FORMAT_H264, 1280, 720, 60, NULL, 0) == 0);
  cb.start(); cb_prepared(1);
}
static void end(void) { cb.cleanup(); }
static int frame(unsigned host_ms, int type) {
  static char data[16]; LENTRY e = {NULL, data, sizeof data, 0};
  DECODE_UNIT du; memset(&du, 0, sizeof du);
  du.presentationTimeMs = host_ms; du.frameType = type; du.fullLength = sizeof data; du.bufferList = &e;
  return cb.submitDecodeUnit(&du);
}
#define P FRAME_TYPE_PFRAME
int main(void) {
  /* 1: mode off. First frame is 0; a repeated or backwards host PTS gets prev+1. */
  begin(NULL, NULL);
  assert(frame(1000, P) == DR_OK && frame(1016, P) == DR_OK && frame(1016, P) == DR_OK && frame(1005, P) == DR_OK);
  assert(rec_ms[0] == 0 && rec_ms[1] == 16 && rec_ms[2] == 17 && rec_ms[3] == 18 && sub_calls == 4);
  end();
  /* 2: nudge. 10 ms/s: ticks at 1, 2, 3 s give a correction of 0, 10, 20 ms. */
  begin("nudge", NULL);
  frame(0, P); frame(1000, P); play_ms = 100;      /* depth 900 */
  esplus_sample_tick(1000); esplus_sample_tick(2000); esplus_sample_tick(3000);
  assert(esplus_correction_ms() == 20);
  frame(1100, P); assert(rec_ms[2] == 1100 - 20);
  end();
  /* a correction larger than the relative PTS clamps at 0, then the PTS still rises */
  setenv("MLENGINE_NUDGE_MAX_MS_PER_S", "1000", 1);
  begin("nudge", NULL);
  frame(0, P); frame(500, P); play_ms = 1;         /* depth 499, target 50 */
  for (int i = 1; i <= 4; i++) esplus_sample_tick(i * 1000);
  assert(esplus_correction_ms() > 510);
  frame(510, P); frame(511, P);
  assert(rec_ms[2] == 501 && rec_ms[3] == 502);
  end(); unsetenv("MLENGINE_NUDGE_MAX_MS_PER_S");
  /* 3: arrival. PTS follows the arrival clock: starts at 0, never goes back. */
  begin("arrival", NULL);
  for (int i = 0; i < 4; i++) { frame(5000, P); usleep(3000); }   /* host PTS is ignored */
  assert(rec_ms[0] == 0);
  for (int i = 1; i < 4; i++) assert(rec_ms[i] >= rec_ms[i - 1]);
  end();
  /* 4: FULL retries */
  begin(NULL, NULL);
  script_set(2, ESPLUSPLAYER_SUBMIT_STATUS_FULL);
  assert(frame(0, P) == DR_OK && sub_calls == 3 && esplus_stats.dropped_full == 0 && idr_calls == 0 && esplus_stats.submitted == 1);
  sub_calls = 0; script_set(4, ESPLUSPLAYER_SUBMIT_STATUS_FULL);
  assert(frame(16, P) == DR_NEED_IDR && sub_calls == 4 && esplus_stats.dropped_full == 1 && idr_calls == 0);
  end();
  /* 5: late-frame cap, at most one skip per tick, never an IDR */
  begin(NULL, "50");
  frame(0, P); frame(200, P); play_ms = 10;        /* depth 190 > 50 */
  esplus_sample_tick(1000); sub_calls = 0;
  assert(frame(216, P) == DR_NEED_IDR && sub_calls == 0 && esplus_stats.dropped_late == 1);
  assert(frame(232, P) == DR_OK && sub_calls == 1);                      /* same tick: sent */
  esplus_sample_tick(1250);
  assert(frame(248, FRAME_TYPE_IDR) == DR_OK && sub_calls == 2 && esplus_stats.dropped_late == 1);   /* IDR: sent */
  assert(frame(264, P) == DR_NEED_IDR && sub_calls == 2 && esplus_stats.dropped_late == 2);                /* new tick: one more skip */
  assert(frame(280, P) == DR_OK && sub_calls == 3);
  end();
  /* 6: any other failure status asks for an IDR */
  begin(NULL, NULL);
  script_set(1, ESPLUSPLAYER_SUBMIT_STATUS_INVALID_PACKET);
  assert(frame(0, P) == DR_NEED_IDR && esplus_stats.submitted == 0);
  end();
  puts("video esplus ok"); return 0;
}

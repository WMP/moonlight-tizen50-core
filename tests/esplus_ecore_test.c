/*
 * Host self-test for mlclient/esplus_player.c: the player/window setup keeps the
 * ecore/ecore_wl2 reference counts balanced on every path. An extra ecore_shutdown()
 * would shut down the .NET host's own ecore loop. EFL and esplusplayer are stubbed.
 * Run: sh tests/build-esplus-ecore-test.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "mldiag.h"
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
int scr_w, scr_h, syncs;   /* screen size the stubbed API reports (0x0 = no output) */
void ecore_wl2_display_screen_size_get(void *x, int *ww, int *hh) { (void)x; *ww = scr_w; *hh = scr_h; }
void ecore_wl2_display_sync(void *x) { (void)x; syncs++; }
static int in_main;   /* 1 while the call_sync stub runs a callback (the real main loop) */
void *ecore_main_loop_thread_safe_call_sync(void *(*cb)(void *), void *data) { in_main = 1; void *r = cb(data); in_main = 0; return r; }
#ifndef NO_OSD_SYMS
static int lower_calls, raise_calls;
void ecore_wl2_window_lower(void *x) { assert(in_main && x); lower_calls++; }
void ecore_wl2_window_raise(void *x) { assert(in_main && x); raise_calls++; }
#endif
/* async: queued, the test runs it with run_async() (inside the stub it would deadlock on s_lock; the real main loop runs it later) */
static void (*async_cb[4])(void *); static void *async_data[4]; static int async_n;
void ecore_main_loop_thread_safe_call_async(void (*cb)(void *), void *data) { assert(async_n < 4); async_cb[async_n] = cb; async_data[async_n++] = data; }
static void run_async(void) { int n = async_n; async_n = 0; for (int i = 0; i < n; i++) async_cb[i](async_data[i]); }
static int pl;
static int create_calls, open_calls, close_calls, stop_calls, addv_calls, create_null;
esplusplayer_handle esplusplayer_create(void) { create_calls++; return create_null ? NULL : (esplusplayer_handle)&pl; }
int esplusplayer_open(esplusplayer_handle h) { (void)h; open_calls++; return 1; }
static void (*close_hook)(void);   /* runs inside a player close: the slow part of a rebuild must not hold s_lock */
int esplusplayer_close(esplusplayer_handle h) { (void)h; close_calls++; if (close_hook) close_hook(); return 1; }
int esplusplayer_stop(esplusplayer_handle h) { (void)h; stop_calls++; return 1; }
static esplusplayer_video_stream_info last_vinfo;
int esplusplayer_add_video_stream_info(esplusplayer_handle h, const esplusplayer_video_stream_info *i) { (void)h; addv_calls++; last_vinfo = *i; return 1; }
static esplusplayer_prepare_async_done_cb cb_prepared;
static esplusplayer_error_cb cb_error;
static esplusplayer_buffer_status_cb cb_buffer;
static esplusplayer_resource_conflicted_cb cb_conflict;
void esplusplayer_set_error_cb(esplusplayer_handle h, esplusplayer_error_cb c) { (void)h; cb_error = c; }
void esplusplayer_set_buffer_status_cb(esplusplayer_handle h, esplusplayer_buffer_status_cb c) { (void)h; cb_buffer = c; }
void esplusplayer_set_resource_conflicted_cb(esplusplayer_handle h, esplusplayer_resource_conflicted_cb c) { (void)h; cb_conflict = c; }
void esplusplayer_set_ready_to_prepare_cb(esplusplayer_handle h, esplusplayer_ready_to_prepare_cb c) { (void)h;(void)c; }
void esplusplayer_set_prepare_async_done_cb(esplusplayer_handle h, esplusplayer_prepare_async_done_cb c) { (void)h; cb_prepared = c; }
static int prep_calls;
int esplusplayer_prepare_async(esplusplayer_handle h) { (void)h; prep_calls++; return 1; }
static uint64_t play_ms; static int play_ok = 1, play_calls;
int esplusplayer_get_playing_time(esplusplayer_handle h, uint64_t *t) { (void)h; play_calls++; *t = play_ms; return play_ok; }
static int start_calls, start_ret = 1;
int esplusplayer_start(esplusplayer_handle h) { (void)h; start_calls++; return start_ret; }
int esplusplayer_get_state(esplusplayer_handle h) { (void)h; return 2; }
static int submit_ret = 4;
static int sp_calls;
int esplusplayer_submit_packet(esplusplayer_handle h, const esplusplayer_es_packet *p) { (void)h;(void)p; sp_calls++; return submit_ret; }
static int disp_w, disp_h, disp_mode = -1, disp_calls;
int esplusplayer_set_ecore_display(esplusplayer_handle h, esplusplayer_display_type t, void *win, int x, int y, int ww, int hh) { (void)h;(void)t;(void)win;(void)x;(void)y; disp_w = ww; disp_h = hh; disp_calls++; return 1; }
int esplusplayer_set_display_mode(esplusplayer_handle h, esplusplayer_display_mode m) { (void)h; disp_mode = (int)m; return 1; }
static int seek_calls; static uint64_t seek_target;
int esplusplayer_seek(esplusplayer_handle h, uint64_t t) { (void)h; seek_calls++; seek_target = t; return 1; }
void esplusplayer_set_seek_done_cb(esplusplayer_handle h, esplusplayer_seek_done_cb c) { (void)h;(void)c; }
static int idr_calls;
void LiRequestIdrFrame(void) { idr_calls++; }
extern esplusplayer_handle esplus_create_open(void);
extern int esplus_set_window(int, int);
extern void esplus_close(void);
extern int esplus_submit(const esplusplayer_es_packet *pkt);
extern int esplus_submit_ep(const esplusplayer_es_packet *pkt, unsigned epoch);
extern void esplus_prepare(void);
extern int esplus_add_video_info(const esplusplayer_video_stream_info *vi);
extern int ml_engine_set_stats_osd(int on);
extern int ml_engine_get_stats_osd(void);
static int hook_ran, hook_ret, hook_sp;
static void vsub(uint64_t pts_ms);
void rebuild_hook_submit(void) {
  esplusplayer_es_packet p = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, NULL, 1, 0, 0}; int c = sp_calls;
  hook_ran++; hook_ret = esplus_submit(&p); hook_sp = sp_calls - c;
}
static void vsub(uint64_t pts_ms) { esplusplayer_es_packet p = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, NULL, 1, pts_ms * 1000000ull, 0}; esplus_submit(&p); }
/* start a session on a stubbed, prepared and started player */
static void begin(void) { esplus_create_open(); cb_prepared(1); esplus_prepare(); play_ms = 0; play_ok = 1; vsub(0); }
int main(int argc, char **argv) {
  (void)argv;
  if (argc > 1) {   /* nosym: built with -DNO_OSD_SYMS, the TV's libecore_wl2 lacks the stacking calls */
    char line[4096]; ecore_cnt = 1;
    assert(ml_engine_set_stats_osd(1) == -1);
    esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); assert(ml_engine_get_stats_osd() == -1);
    assert(ml_engine_set_stats_osd(0) == -1 && ml_engine_get_stats_osd() == -1);   /* mid-stream toggle: recorded, nothing ends */
    assert(ml_engine_set_stats_osd(1) == -1 && ml_engine_get_stats_osd() == -1);
    mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line); assert(strstr(line, " osd=1/nosym"));
    esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0);
    puts("osd nosym ok"); return 0;
  }
  /* the host app already holds one ecore reference */
  ecore_cnt = 1;
  /* 1: close without any window (setup failed before set_window) must not shut down ecore */
  esplus_create_open(); esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0);
  /* 2: success, then close */
  esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); assert(ecore_cnt == 2 && wl2_cnt == 1);
  /* #17: the API reports 0x0 (no output) -> 1920x1080 fallback, after a sync; letterboxed */
  assert(disp_w == 1920 && disp_h == 1080 && disp_mode == ESPLUSPLAYER_DISPLAY_MODE_LETTER_BOX && syncs == 1);
  esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0);
  /* #17: the API reports the screen -> the display area is the screen, not the stream */
  scr_w = 3840; scr_h = 2160; esplus_create_open(); assert(esplus_set_window(1280, 720) == 0);
  assert(disp_w == 3840 && disp_h == 2160 && syncs == 2);
  esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0); scr_w = scr_h = 0;
  /* 3: no display (service without display access) -> error, balanced, close is a no-op */
  disp_ok = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == -1); assert(ecore_cnt == 1 && wl2_cnt == 0);
  esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0); disp_ok = 1;
  /* 4: no window */
  win_ok = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == -1); esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0);
  /* 5: no player */
  assert(esplus_set_window(1280, 720) == -1 && ecore_cnt == 1);
  /* 6: counters. Only video SUCCESS counts as submitted; callbacks bump the rest. */
  win_ok = 1; esplus_create_open();
  assert(esplus_stats.submitted == 0 && cb_error && cb_buffer && cb_conflict);
  esplusplayer_es_packet v = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, NULL, 1, 40000000ull, 0}, a = v;
  a.type = ESPLUSPLAYER_STREAM_TYPE_AUDIO;
  esplus_submit(&v); esplus_submit(&a); submit_ret = 3; esplus_submit(&v); submit_ret = 4;
  assert(esplus_stats.submitted == 1);
  cb_buffer(1, 0); cb_buffer(1, 1); cb_buffer(1, 1); cb_error(0x42); cb_error(0x43);
  assert(esplus_stats.underruns == 1 && esplus_stats.overruns == 2 && esplus_stats.errors == 2 && esplus_stats.last_error == 0x43);
  cb_conflict(); assert(esplus_stats.recovery_state == 2);
  esplus_close(); esplus_create_open(); assert(esplus_stats.submitted == 0 && esplus_stats.recovery_state == 0);   /* new session */
  esplus_close();
  /* a: depth and fps from the sampler tick (thread off) */
  setenv("MLENGINE_DEPTH_POLL_MS", "0", 1);
  begin();
  esplus_sample_tick(1000);                        /* first tick starts the fps second */
  for (int i = 1; i <= 60; i++) vsub(i * 16);      /* 60 + the begin() packet = 61 */
  play_ms = 500;
  esplus_sample_tick(2000);
  assert(esplus_stats.playing_ms == 500 && esplus_stats.depth_ms == 960 - 500 && esplus_stats.audio_depth_ms == 0);
  assert(esplus_stats.fps_submitted == 60);
  play_ok = 0; play_ms = 9999; esplus_sample_tick(3000); assert(esplus_stats.playing_ms == 500);   /* failed read: unchanged */
  esplus_close();
  /* b: nudge. Correction grows by at most nudge_max*dt; inside the hysteresis it holds. */
  setenv("MLENGINE_LATENCY_MODE", "nudge", 1); setenv("MLENGINE_CATCHUP_MS", "0", 1);   /* depth 900 here: no catch-up seek */
  begin(); vsub(1000); play_ms = 100;               /* depth 900, target 50 */
  esplus_sample_tick(1000); assert(esplus_correction_ms() == 0);     /* first tick: no change */
  esplus_sample_tick(2000); assert(esplus_correction_ms() == 10);    /* 10 ms/s * 1 s */
  esplus_sample_tick(2500); assert(esplus_correction_ms() == 15);    /* 5 ms more for 0.5 s */
  esplus_sample_tick(3500); assert(esplus_correction_ms() == 25);
  play_ms = 940;                                    /* depth 60: err 10 within hysteresis 25 */
  esplus_sample_tick(4500); assert(esplus_correction_ms() == 25);
  play_ms = 1200;                                   /* depth -200: pulls back */
  esplus_sample_tick(5500); assert(esplus_correction_ms() == 15);
  esplus_close(); assert(esplus_correction_ms() == 0);
  unsetenv("MLENGINE_LATENCY_MODE");
  esplus_close();
  /* c: watchdog. RECOVER_MAX=0 (default): a stall never seeks. */
  begin(); vsub(1000); play_ms = 100;
  esplus_sample_tick(1000); vsub(1010); esplus_sample_tick(1800); vsub(1020); esplus_sample_tick(5000);
  assert(seek_calls == 0 && esplus_stats.recoveries == 0);
  esplus_close();
  /* RECOVER_MAX=2: seek behind the last PTS + forced IDR; 2 s spacing; then give up */
  setenv("MLENGINE_RECOVER_MAX", "2", 1);
  begin(); idr_calls = 0; vsub(1000); play_ms = 100;
  esplus_sample_tick(1000); vsub(1010); esplus_sample_tick(1500); assert(seek_calls == 0);   /* not stalled long enough */
  esplus_sample_tick(1800);                                  /* stalled 800 ms while submitting */
  assert(seek_calls == 1 && seek_target == 1010 - 250 && esplus_take_need_idr() == 1 && esplus_take_need_idr() == 0 && esplus_stats.recoveries == 1 && esplus_stats.recovery_state == 0);
  vsub(1020); esplus_sample_tick(2600); assert(seek_calls == 1);   /* within 2 s of the last recovery */
  vsub(1030); esplus_sample_tick(3900); assert(seek_calls == 2 && esplus_take_need_idr() == 1 && seek_target == 1030 - 250);
  vsub(1040); esplus_sample_tick(4700); assert(seek_calls == 2);
  vsub(1050); esplus_sample_tick(6000); assert(seek_calls == 2 && esplus_stats.recovery_state == 2);   /* gave up */
  vsub(1060); esplus_sample_tick(9000); assert(seek_calls == 2 && esplus_stats.recoveries == 2);
  esplus_close();
  /* an error callback recovers on the next tick, not in the callback */
  seek_calls = 0; begin(); vsub(1000); play_ms = 100; esplus_sample_tick(1000);
  cb_error(7); assert(seek_calls == 0); esplus_sample_tick(1100); assert(seek_calls == 1);
  cb_error(8); esplus_sample_tick(1500); assert(seek_calls == 1);   /* too soon: the error stays pending */
  esplus_sample_tick(3200); assert(seek_calls == 2);
  esplus_close();
  /* d: after a resource conflict nothing seeks */
  seek_calls = 0; begin(); vsub(1000); play_ms = 100; esplus_sample_tick(1000);
  cb_conflict(); assert(esplus_stats.recovery_state == 2);
  vsub(1010); cb_error(7); esplus_sample_tick(2000); assert(seek_calls == 0);
  esplus_close();
  unsetenv("MLENGINE_RECOVER_MAX");
  /* c2: catch-up: one seek to the last PTS when depth stays above catchup_ms on two ticks */
  setenv("MLENGINE_CATCHUP_MS", "150", 1); setenv("MLENGINE_STALL_MS", "700", 1);
  seek_calls = 0; begin(); vsub(1000); play_ms = 100; esplus_sample_tick(1000);   /* depth 900: first hit */
  assert(seek_calls == 0);
  esplus_sample_tick(1250);
  assert(seek_calls == 1 && seek_target == 1000 && esplus_take_need_idr() == 1 && esplus_correction_ms() == 0);
  esplus_sample_tick(1500); esplus_sample_tick(1750); assert(seek_calls == 1);   /* once per session */
  /* e: the pause after the seek is no stall (same playing time, less than stall_ms) */
  setenv("MLENGINE_RECOVER_MAX", "2", 1); esplus_close(); seek_calls = 0; begin(); vsub(1000); play_ms = 100;
  esplus_sample_tick(1000); esplus_sample_tick(1250); assert(seek_calls == 1);
  vsub(1010); esplus_sample_tick(1900);   /* 650 ms after the seek, below stall_ms 700: no recovery seek */
  assert(seek_calls == 1 && esplus_stats.recoveries == 0);
  vsub(1020); esplus_sample_tick(1960); assert(seek_calls == 2 && esplus_stats.recoveries == 1);   /* control: a real stall still recovers */
  unsetenv("MLENGINE_RECOVER_MAX");
  esplus_close();
  /* b2: a single spike does not seek */
  seek_calls = 0; begin(); vsub(1000); play_ms = 100; esplus_sample_tick(1000);
  play_ms = 900; esplus_sample_tick(1250); play_ms = 100; esplus_sample_tick(1500);
  assert(seek_calls == 0);
  esplus_close();
  /* d2: depth below the threshold */
  seek_calls = 0; begin(); vsub(1000); play_ms = 900; esplus_sample_tick(1000); esplus_sample_tick(1250); esplus_sample_tick(1500);
  assert(seek_calls == 0);
  esplus_close();
  /* c3: off */
  setenv("MLENGINE_CATCHUP_MS", "0", 1);
  seek_calls = 0; begin(); vsub(1000); play_ms = 100; esplus_sample_tick(1000); esplus_sample_tick(1250); esplus_sample_tick(1500);
  assert(seek_calls == 0);
  esplus_close();
  /* f: prepare done with ok=0 still starts the player, once; a failed start is not retried */
  start_calls = 0; start_ret = 1; esplus_create_open(); cb_prepared(0); esplus_prepare(); vsub(0); vsub(16);
  assert(start_calls == 1);
  esplus_close();
  start_calls = 0; start_ret = 0; esplus_create_open(); cb_prepared(0); esplus_prepare(); vsub(0); vsub(16); vsub(32);
  assert(start_calls == 1);
  esplus_close(); start_ret = 1;
  /* g: start watchdog */
  setenv("MLENGINE_DEPTH_POLL_MS", "0", 1);
  start_ret = 0; esplus_create_open(); cb_prepared(0); esplus_prepare();
  esplus_sample_tick(1000); esplus_sample_tick(9000); assert(esplus_stats.fatal == 0);   /* no video submit yet */
  vsub(0); esplus_sample_tick(10000); esplus_sample_tick(14999); assert(esplus_stats.fatal == 0);
  esplus_sample_tick(15001); assert(esplus_stats.fatal == ML_ERR_PLAYER_NOT_STARTED);
  esplus_close();
  esplus_create_open(); assert(esplus_stats.fatal == 0);   /* prepare never done */
  vsub(0); esplus_sample_tick(1000); esplus_sample_tick(6001); assert(esplus_stats.fatal == ML_ERR_PLAYER_NOT_STARTED);
  esplus_close();
  setenv("MLENGINE_START_TIMEOUT_MS", "0", 1);
  esplus_create_open(); vsub(0); esplus_sample_tick(1000); esplus_sample_tick(900000); assert(esplus_stats.fatal == 0);
  esplus_close(); unsetenv("MLENGINE_START_TIMEOUT_MS"); start_ret = 1;
  /* h: fatal stall (recovery off) */
  begin(); vsub(1000); play_ms = 100; esplus_sample_tick(1000);
  vsub(1010); esplus_sample_tick(10000); assert(esplus_stats.fatal == 0);
  vsub(1020); esplus_sample_tick(11001); assert(esplus_stats.fatal == ML_ERR_PLAYER_STALLED);
  esplus_close();
  begin(); vsub(1000); play_ms = 100; esplus_sample_tick(1000);
  for (int i = 1; i <= 30; i++) { play_ms += 100; vsub(1000 + i); esplus_sample_tick(1000 + i * 1000); }
  assert(esplus_stats.fatal == 0);
  esplus_close();
  /* i: video-only player: prepare on the FIRST esplus_prepare() call, later calls do nothing; the sampler
     starts with it. esplus_submit passes every packet type to the player (no audio special case). */
  setenv("MLENGINE_DEPTH_POLL_MS", "50", 1);
  prep_calls = 0; esplus_create_open(); play_calls = 0;
  cb_prepared(1); vsub(0); usleep(150000); assert(prep_calls == 0 && play_calls == 0);   /* not prepared: no sampler */
  esplus_prepare(); assert(prep_calls == 1); usleep(150000); assert(play_calls >= 1);
  esplus_prepare(); assert(prep_calls == 1);
  { int c = sp_calls; esplusplayer_es_packet a = {ESPLUSPLAYER_STREAM_TYPE_AUDIO, NULL, 1, 0, 0}; assert(esplus_submit(&a) == 4 && sp_calls == c + 1); }
  esplus_close();
  esplus_create_open(); esplus_prepare(); assert(prep_calls == 2);   /* a new session prepares again */
  esplus_close();
  setenv("MLENGINE_DEPTH_POLL_MS", "0", 1);
  /* j: playing time cannot be read while video arrives: counts as frozen */
  begin(); play_ok = 0; esplus_sample_tick(1000);
  vsub(10); esplus_sample_tick(11000); assert(esplus_stats.fatal == 0);
  vsub(20); esplus_sample_tick(11001); assert(esplus_stats.fatal == ML_ERR_PLAYER_STALLED);
  esplus_close();
  begin(); play_ok = 0; esplus_sample_tick(1000);   /* no new video submit after the first tick */
  esplus_sample_tick(900000); assert(esplus_stats.fatal == 0);
  esplus_close(); play_ok = 1;
  /* started, but the playing time stays 0 while frames arrive (first frame frozen, #22) */
  begin(); esplus_sample_tick(1000); vsub(16); esplus_sample_tick(11000); assert(esplus_stats.fatal == 0);
  vsub(32); esplus_sample_tick(11001); assert(esplus_stats.fatal == ML_ERR_PLAYER_STALLED);
  esplus_close();
  /* a static picture with no new frames is not a stall */
  begin(); play_ms = 100; esplus_sample_tick(1000); esplus_sample_tick(60000); assert(esplus_stats.fatal == 0);
  esplus_close();
  /* m: IDR retry at the start */
  { char line[4096];
    /* IDR retry at the start: only while no video packet reached the player */
    mldiag_reset(NULL);
    esplus_create_open(); esplus_prepare(); idr_calls = 0;
    esplus_sample_tick(1000); esplus_sample_tick(1300); assert(idr_calls == 0);   /* t0 = 1000; not more than 300 ms yet */
    esplus_sample_tick(1301); assert(idr_calls == 1);
    esplus_sample_tick(1800); assert(idr_calls == 1);                             /* < 500 ms after the last retry */
    esplus_sample_tick(1801); assert(idr_calls == 2);
    for (int i = 0; i < 20; i++) esplus_sample_tick(2400 + i * 600);
    assert(idr_calls == 10);                                                      /* at most 10 */
    mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " idr_retry=10 |"));
    esplus_close();
    esplus_create_open(); esplus_prepare(); idr_calls = 0;
    esplus_sample_tick(1000); vsub(0); esplus_sample_tick(2000); esplus_sample_tick(9000); assert(idr_calls == 0);   /* video submitted: none */
    esplus_close();
    esplus_create_open(); idr_calls = 0;   /* not prepared yet: no retry */
    esplus_sample_tick(1000); esplus_sample_tick(9000); assert(idr_calls == 0);
    esplus_close();
  }
  /* n: player rebuild (MLENGINE_PLAYER_REBUILD_MAX default 1). It needs a window and the cached video info. */
  { esplusplayer_video_stream_info vi = {NULL, 0, 1, 1280, 720, 1280, 720, 60, 1};
    scr_w = scr_h = 0;
    /* start failed after prepare done: one rebuild on the next tick, display + prepare later on the main loop */
    start_ret = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); assert(esplus_add_video_info(&vi) == 1 && addv_calls >= 1);
    cb_prepared(1); esplus_prepare(); vsub(0);   /* start() fails */
    assert(start_calls >= 0 && esplus_stats.fatal == 0);
    int c0 = create_calls, o0 = open_calls, cl0 = close_calls, st0 = stop_calls, a0 = addv_calls, d0 = disp_calls, p0 = prep_calls; async_n = 0;
    start_calls = 0;
    /* D: a submit from inside the slow part (stop/close of the old player) must not block on s_lock, and finds no player */
    { extern void rebuild_hook_submit(void); close_hook = rebuild_hook_submit; }
    esplus_sample_tick(1000); close_hook = NULL;
    assert(hook_ran == 1 && hook_ret == ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED && hook_sp == 0);
    /* C: callbacks of the closed player (cb_live == 0 until the new player's prepare_async) change nothing */
    { unsigned e0 = esplus_stats.errors; int cp = start_calls;
      cb_error(0x99); cb_conflict(); cb_prepared(1);
      assert(esplus_stats.errors == e0 && esplus_stats.recovery_state == 0);
      vsub(5); assert(start_calls == cp); }   /* a late prepared callback did not mark the new player prepared */
    assert(create_calls == c0 + 1 && open_calls == o0 + 1 && close_calls == cl0 + 1 && stop_calls == st0 + 1 && addv_calls == a0 + 1);
    assert(last_vinfo.width == 1280 && last_vinfo.codec_data == NULL);
    assert(disp_calls == d0 && prep_calls == p0 && async_n == 1);          /* part 2 is queued, not run in the tick */
    assert(esplus_take_need_idr() == 0);                                    /* the IDR is asked only after prepare_async */
    /* B: a submit with the epoch read before the display step is refused; one with the new epoch passes */
    { unsigned ep = esplus_epoch(); esplusplayer_es_packet p = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, NULL, 1, 0, 0};
      run_async(); assert(disp_calls == d0 + 1 && disp_w == 1920 && disp_h == 1080 && prep_calls == p0 + 1);
      assert(esplus_epoch() == ep + 1);
      int c = sp_calls; assert(esplus_submit_ep(&p, ep) == ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED && sp_calls == c);
      assert(esplus_submit_ep(&p, ep + 1) == submit_ret && sp_calls == c + 1); }
    { unsigned e0 = esplus_stats.errors; cb_error(0x98); assert(esplus_stats.errors == e0 + 1); }   /* live again */
    assert(esplus_take_need_idr() == 1);
    { char line[4096]; mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line); assert(strstr(line, " rebuilds=1 ")); }
    esplus_sample_tick(2000); assert(create_calls == c0 + 1);                 /* max 1: no second rebuild */
    start_ret = 1; cb_prepared(1); vsub(16); assert(start_calls == 1 && esplus_stats.fatal == 0);   /* the new player is started on a submit */
    esplus_close();
    /* the fatal watchdogs act as before after the limit: start failed again */
    start_ret = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); esplus_add_video_info(&vi);
    cb_prepared(1); esplus_prepare(); vsub(0); esplus_sample_tick(1000); run_async();      /* rebuild */
    cb_prepared(1); vsub(16); esplus_sample_tick(2000); esplus_sample_tick(7000); assert(esplus_stats.fatal == 0);   /* start_t0 = 2000 */
    esplus_sample_tick(7001); assert(esplus_stats.fatal == ML_ERR_PLAYER_NOT_STARTED);
    esplus_close(); start_ret = 1;
    /* started, but the playing time cannot be read while video arrives: no rebuild before stall_ms, one after;
       then the fatal stall fires fatal_stall_ms after the rebuild */
    esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); esplus_add_video_info(&vi);
    cb_prepared(1); esplus_prepare(); play_ok = 0; vsub(0); c0 = create_calls;
    esplus_sample_tick(1000); vsub(16); esplus_sample_tick(1999); assert(create_calls == c0);   /* 999 ms: not stall_ms, not 2 s */
    esplus_sample_tick(2999); assert(create_calls == c0);                    /* 1999 ms past s_stall_t */
    esplus_sample_tick(3000); assert(create_calls == c0);                    /* 2000 ms: not yet (more than 2 s) */
    esplus_sample_tick(3001); assert(create_calls == c0 + 1);
    assert(esplus_take_rebase() == 0);                                       /* set only after the display step */
    run_async(); assert(esplus_take_rebase() == 1 && esplus_take_rebase() == 0);
    cb_prepared(1); vsub(32); esplus_sample_tick(3800);          /* started again, time still unreadable */
    vsub(48); esplus_sample_tick(9000); assert(create_calls == c0 + 1);       /* no second rebuild */
    vsub(64); esplus_sample_tick(13800); assert(esplus_stats.fatal == 0);
    vsub(80); esplus_sample_tick(13801); assert(esplus_stats.fatal == ML_ERR_PLAYER_STALLED);
    esplus_close(); play_ok = 1;
    /* F: with MLENGINE_FATAL_STALL_MS=1500 the rebuild runs at > 1500 ms (not the fatal, not at 2 s) */
    setenv("MLENGINE_FATAL_STALL_MS", "1500", 1);
    esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); esplus_add_video_info(&vi);
    cb_prepared(1); esplus_prepare(); play_ok = 0; vsub(0); c0 = create_calls;
    esplus_sample_tick(1000); vsub(16); esplus_sample_tick(2500); assert(create_calls == c0 && esplus_stats.fatal == 0);
    esplus_sample_tick(2501); assert(create_calls == c0 + 1 && esplus_stats.fatal == 0);
    esplus_close(); play_ok = 1; unsetenv("MLENGINE_FATAL_STALL_MS");
    /* MLENGINE_PLAYER_REBUILD_MAX=0: no rebuild */
    setenv("MLENGINE_PLAYER_REBUILD_MAX", "0", 1);
    start_ret = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); esplus_add_video_info(&vi);
    cb_prepared(1); esplus_prepare(); vsub(0); c0 = create_calls; async_n = 0;
    esplus_sample_tick(1000); esplus_sample_tick(6001); assert(create_calls == c0 && async_n == 0 && esplus_stats.fatal == ML_ERR_PLAYER_NOT_STARTED);
    esplus_close(); start_ret = 1;
    unsetenv("MLENGINE_PLAYER_REBUILD_MAX");
    /* stale part 2: close before the main loop runs it */
    start_ret = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); esplus_add_video_info(&vi);
    cb_prepared(1); esplus_prepare(); vsub(0); async_n = 0; esplus_sample_tick(1000); assert(async_n == 1);
    d0 = disp_calls; p0 = prep_calls; esplus_close(); run_async(); assert(disp_calls == d0 && prep_calls == p0);
    start_ret = 1;
    /* a part 2 that runs before the close is not stale; the close then stops and closes the rebuilt player */
    start_ret = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); esplus_add_video_info(&vi);
    cb_prepared(1); esplus_prepare(); vsub(0); async_n = 0; esplus_sample_tick(1000); d0 = disp_calls; run_async(); assert(disp_calls == d0 + 1);
    cl0 = close_calls; esplus_close(); assert(close_calls == cl0 + 1);
    start_ret = 1;
    /* a stale step of an older session: the new session's player is not touched */
    start_ret = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); esplus_add_video_info(&vi);
    cb_prepared(1); esplus_prepare(); vsub(0); async_n = 0; esplus_sample_tick(1000);
    esplus_close(); esplus_create_open(); assert(esplus_set_window(1280, 720) == 0);
    d0 = disp_calls; p0 = prep_calls; run_async(); assert(disp_calls == d0 && prep_calls == p0);
    esplus_close(); start_ret = 1;
    /* no window (set_window never called) or no cached video info: no rebuild */
    start_ret = 0; esplus_create_open(); esplus_add_video_info(&vi);
    cb_prepared(1); esplus_prepare(); vsub(0); c0 = create_calls; esplus_sample_tick(1000); assert(create_calls == c0 && async_n == 0);
    esplus_close();
    esplus_create_open(); assert(esplus_set_window(1280, 720) == 0);
    cb_prepared(1); esplus_prepare(); vsub(0); c0 = create_calls; esplus_sample_tick(1000); assert(create_calls == c0);
    esplus_close(); start_ret = 1;
    /* create() fails during the rebuild: the session ends at once (no tick branch runs without a player) */
    start_ret = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); esplus_add_video_info(&vi);
    cb_prepared(1); esplus_prepare(); vsub(0); async_n = 0; create_null = 1; esplus_sample_tick(1000); create_null = 0;
    assert(esplus_stats.fatal == ML_ERR_PLAYER_NOT_STARTED && async_n == 0);
    esplus_sample_tick(2000);   /* no player: nothing crashes */
    /* A: no player: submit answers NOT_PREPARED and reaches neither start() nor submit_packet() */
    { int c = sp_calls, s0 = start_calls; cb_prepared(1); vsub(5); assert(sp_calls == c && start_calls == s0); }
    esplus_close(); start_ret = 1;
  }
  /* e: sampler thread starts at prepare, stops promptly at close */
  setenv("MLENGINE_DEPTH_POLL_MS", "50", 1);
  begin(); play_calls = 0;
  usleep(150000); assert(play_calls >= 1);
  esplus_close(); int n = play_calls; usleep(120000); assert(play_calls == n);
  unsetenv("MLENGINE_DEPTH_POLL_MS");
#ifndef NO_OSD_SYMS
  /* o: stats overlay (window stacking) */
  { esplusplayer_video_stream_info vi = {NULL, 0, 1, 1280, 720, 1280, 720, 60, 1}; char line[4096];
    assert(lower_calls == 0 && raise_calls == 0);   /* default: a new window is not touched */
    esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); assert(lower_calls == 0 && raise_calls == 0 && ml_engine_get_stats_osd() == 0);
    esplus_close();
    assert(ml_engine_set_stats_osd(1) == 0 && lower_calls == 0 && ml_engine_get_stats_osd() == 0);   /* no window: stored */
    start_ret = 0; esplus_create_open(); assert(esplus_set_window(1280, 720) == 0);
    assert(lower_calls == 1 && raise_calls == 0 && ml_engine_get_stats_osd() == 1);
    assert(ml_engine_set_stats_osd(0) == 0 && raise_calls == 1 && lower_calls == 1 && ml_engine_get_stats_osd() == 2);
    assert(ml_engine_set_stats_osd(1) == 0 && lower_calls == 2 && ml_engine_get_stats_osd() == 1);
    { int c0 = create_calls; esplus_add_video_info(&vi); cb_prepared(1); esplus_prepare(); vsub(0); esplus_sample_tick(1000); run_async();   /* rebuild */
      assert(create_calls == c0 + 1); }
    assert(lower_calls == 2 && raise_calls == 1 && ml_engine_get_stats_osd() == 1);
    mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line); assert(strstr(line, " osd=1/lowered"));
    esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0 && ml_engine_get_stats_osd() == 0); start_ret = 1;
    esplus_create_open(); assert(esplus_set_window(1280, 720) == 0); assert(lower_calls == 3 && ml_engine_get_stats_osd() == 1);   /* want remembered */
    esplus_close(); assert(ecore_cnt == 1 && wl2_cnt == 0);
    ml_engine_set_stats_osd(0);
  }
#endif
  puts("ecore balance ok"); return 0;
}

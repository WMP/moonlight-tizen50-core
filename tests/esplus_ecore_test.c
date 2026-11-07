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
static int disp_w, disp_h, disp_mode = -1;
int esplusplayer_set_ecore_display(esplusplayer_handle h, esplusplayer_display_type t, void *win, int x, int y, int ww, int hh) { (void)h;(void)t;(void)win;(void)x;(void)y; disp_w = ww; disp_h = hh; return 1; }
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
extern void esplus_prepare(void);
static void vsub(uint64_t pts_ms) { esplusplayer_es_packet p = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, NULL, 1, pts_ms * 1000000ull, 0}; esplus_submit(&p); }
/* start a session on a stubbed, prepared and started player */
static void begin(void) { esplus_create_open(); cb_prepared(1); esplus_prepare(); play_ms = 0; play_ok = 1; vsub(0); }
int main(void) {
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
  setenv("MLENGINE_LATENCY_MODE", "nudge", 1);
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
  /* m: A/V drift = (audio written - playing time) - baseline; the baseline is taken at the first tick with
     playing time > 0; about one sample per second; the max |drift| goes to the ML-ERROR line */
  { char line[4096];
    mldiag_reset(NULL); mldiag_aout_open(0);
    begin(); vsub(1000);
    esplus_stats.audio_written_ms = 1000; play_ms = 0; esplus_sample_tick(1000);   /* playing time 0: no baseline yet */
    play_ms = 500; esplus_sample_tick(2000);                        /* baseline = 1000 - 500 */
    esplus_stats.audio_written_ms = 1500; play_ms = 1000; esplus_sample_tick(3000);   /* drift 0 */
    mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, "drift_max_ms=0 |"));
    esplus_stats.audio_written_ms = 2400; play_ms = 1500; esplus_sample_tick(3500);   /* less than 1 s since the last sample: skipped */
    mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, "drift_max_ms=0 |"));
    esplus_sample_tick(4000);                                       /* (2400-1500) - 500 = 400 */
    mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, "drift_max_ms=400 |"));
    esplus_stats.audio_written_ms = 1000; play_ms = 2000; esplus_sample_tick(5000);   /* (1000-2000) - 500 = -1500 */
    esplus_stats.audio_written_ms = 2500; play_ms = 2500; esplus_sample_tick(6000);   /* -500: the max stays */
    mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, "drift_max_ms=1500 |"));
    esplus_close();
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
  /* e: sampler thread starts at prepare, stops promptly at close */
  setenv("MLENGINE_DEPTH_POLL_MS", "50", 1);
  begin(); play_calls = 0;
  usleep(150000); assert(play_calls >= 1);
  esplus_close(); int n = play_calls; usleep(120000); assert(play_calls == n);
  unsetenv("MLENGINE_DEPTH_POLL_MS");
  puts("ecore balance ok"); return 0;
}

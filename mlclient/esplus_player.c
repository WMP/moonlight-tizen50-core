/*
 * esplus_player.c - one shared esplusplayer instance for the VIDEO sink (the TV allows
 * only one player per process; a second create() deletes the first). The player has
 * only a video stream: platform limitation: the TV player rejects engine-fed audio
 * streams (both Opus and raw PCM), so audio is decoded by the engine (audio_opus.c) and handed to the host, which plays it with Web Audio in the UI.
 * moonlight-common-c calls the sinks in this order (connection stages in Connection.c):
 * video setup and video start (stage 9), then audio init and audio start (stage 10).
 * So: video setup creates+opens the player, attaches the window (it has the dimensions)
 * and adds the video stream info; prepare runs on the first esplus_prepare() call (video
 * start); start runs from the first submit after prepare_async_done.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esplus_stats.h"
#include "esplusplayer_tv50.h"
#include "mldiag.h"
#include "mltrace.h"

/* EFL opaque decls (same as esppplay.c). */
int ecore_init(void);
int ecore_shutdown(void);
int ecore_wl2_init(void);
int ecore_wl2_shutdown(void);
void *ecore_wl2_display_connect(const char *name);
void ecore_wl2_display_disconnect(void *d);
void *ecore_wl2_window_new(void *d, void *parent, int x, int y, int w, int h);
void ecore_wl2_window_show(void *w);
void ecore_wl2_window_alpha_set(void *w, unsigned char alpha);
void ecore_wl2_window_free(void *w);
void ecore_wl2_display_flush(void *d);
/* never the _sync variant from the sampler (it holds s_lock; esplus_close may run on the main loop and join the sampler) */
void ecore_main_loop_thread_safe_call_async(void (*cb)(void *data), void *data);
/* weak: NULL if the TV's libecore_wl2 does not export them */
void ecore_wl2_display_screen_size_get(void *d, int *w, int *h) __attribute__((weak));
void ecore_wl2_display_sync(void *d) __attribute__((weak));   /* roundtrip: the outputs are known after it */
void ecore_wl2_window_lower(void *w) __attribute__((weak));
void ecore_wl2_window_raise(void *w) __attribute__((weak));
void *ecore_main_loop_thread_safe_call_sync(void *(*cb)(void *data), void *data);

extern void LiRequestIdrFrame(void);   /* moonlight-common-c: only sets an event, thread-safe */

#define PLOG(...) fprintf(stderr, "[esplus] " __VA_ARGS__)

static esplusplayer_handle s_pl;
static void *s_disp, *s_win;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int s_prepared, s_prepare_ok, s_started;   /* s_started: start() succeeded */
static int s_prepare_called;   /* prepare_async was started this session, under s_lock */
static int s_start_tried;   /* start() is tried once per session, under s_lock */

esplus_stats_t esplus_stats;
esplus_cfg_t esplus_cfg;
static volatile int s_conflicted;   /* set by the resource-conflicted callback; cleared per session */

static int mlc_env_int(const char *name, int def) {
  const char *v = getenv(name);
  char *end;
  if (!v || !*v) return def;
  long n = strtol(v, &end, 10);
  return *end || n < 0 ? def : (int)n;   /* every knob is a non-negative amount */
}

static void load_cfg(void) {
  const char *m = getenv("MLENGINE_LATENCY_MODE");
  esplus_cfg.latency_mode = !m ? ESPLUS_LAT_OFF : !strcmp(m, "nudge") ? ESPLUS_LAT_NUDGE
                          : !strcmp(m, "arrival") ? ESPLUS_LAT_ARRIVAL : ESPLUS_LAT_OFF;
  esplus_cfg.target_depth_ms = mlc_env_int("MLENGINE_TARGET_DEPTH_MS", 50);
  esplus_cfg.nudge_max_ms_per_s = mlc_env_int("MLENGINE_NUDGE_MAX_MS_PER_S", 10);
  esplus_cfg.drop_after_ms = mlc_env_int("MLENGINE_DROP_AFTER_MS", 0);
  esplus_cfg.depth_poll_ms = mlc_env_int("MLENGINE_DEPTH_POLL_MS", 250);
  esplus_cfg.stall_ms = mlc_env_int("MLENGINE_STALL_MS", 700);
  /* 0 = never recover. Recommended 3, once validated on a TV. */
  esplus_cfg.recover_max = mlc_env_int("MLENGINE_RECOVER_MAX", 0);
  esplus_cfg.recover_back_ms = mlc_env_int("MLENGINE_RECOVER_BACK_MS", 250);
  esplus_cfg.start_timeout_ms = mlc_env_int("MLENGINE_START_TIMEOUT_MS", 5000);   /* 0 = off */
  esplus_cfg.fatal_stall_ms = mlc_env_int("MLENGINE_FATAL_STALL_MS", 10000);   /* 0 = off */
  esplus_cfg.rebuild_max = mlc_env_int("MLENGINE_PLAYER_REBUILD_MAX", 1);   /* 0 = off */
  esplus_cfg.catchup_ms = mlc_env_int("MLENGINE_CATCHUP_MS", 150);   /* 0 = off */
  MLT("cfg latency=%d target=%d nudge=%d drop_after=%d poll=%d stall=%d recover_max=%d back=%d start_timeout=%d fatal_stall=%d rebuild_max=%d catchup=%d",
      esplus_cfg.latency_mode, esplus_cfg.target_depth_ms, esplus_cfg.nudge_max_ms_per_s, esplus_cfg.drop_after_ms,
      esplus_cfg.depth_poll_ms, esplus_cfg.stall_ms, esplus_cfg.recover_max, esplus_cfg.recover_back_ms,
      esplus_cfg.start_timeout_ms, esplus_cfg.fatal_stall_ms, esplus_cfg.rebuild_max, esplus_cfg.catchup_ms);
}

static uint64_t s_last_video_pts_ms;   /* under s_lock */
static uint64_t s_fps_t0;     /* start of the current fps second; 0 = not started */
static uint32_t s_fps_sub0;

/* set by recover(): the video sink returns DR_NEED_IDR for its next non-IDR frame, so
   moonlight-common-c drops the frames that reference the flushed ones and asks for an IDR */
static volatile int s_need_idr;
int esplus_take_need_idr(void) { return __atomic_exchange_n(&s_need_idr, 0, __ATOMIC_RELAXED); }
/* set after a player rebuild: the video sink restarts its PTS at 0 for the fresh player */
static volatile int s_rebase;
int esplus_take_rebase(void) { return __atomic_exchange_n(&s_rebase, 0, __ATOMIC_RELAXED); }

/* Epoch: bumped under s_lock when a rebuild arms the fresh player (flags above). The video sink reads it BEFORE it takes
   the flags and submits with it (esplus_submit_ep), so a frame that was cut for the old player is refused. */
static unsigned s_epoch;
unsigned esplus_epoch(void) { return __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE); }
/* 1 while the callbacks belong to the current player. The callbacks have no handle: a late one of a closed player must not
   change the state of the new one. 0 from the start of a rebuild until its prepare_async, and after esplus_close. */
static volatile int s_cb_live;

static int64_t s_corr_us;            /* nudge correction, microseconds, under s_lock */
static volatile int32_t s_corr_ms;   /* rounded, read lock-free by the video sink */
static uint64_t s_tick_prev_ms;      /* previous successful tick; 0 = none */
static volatile uint32_t s_tick_seq;

int32_t esplus_correction_ms(void) { return s_corr_ms; }
uint32_t esplus_sample_seq(void) { return s_tick_seq; }


/* Runtime target depth (ml_engine_set_target_depth_ms): engine lifetime, never reset per session; -1 = not set.
   esplus_cfg stays read-only after load_cfg; this override wins over it. */
static volatile int s_target_set = -1;
#define TARGET_MIN_MS 20
#define TARGET_MAX_MS 500

/* A set target also turns on nudge pacing when the env mode is off; otherwise it would do nothing on a TV. */
int esplus_lat_mode(void) {
  if (esplus_cfg.latency_mode == ESPLUS_LAT_OFF && __atomic_load_n(&s_target_set, __ATOMIC_RELAXED) >= 0) return ESPLUS_LAT_NUDGE;
  return esplus_cfg.latency_mode;
}
static int eff_target_ms(void) {
  int t = __atomic_load_n(&s_target_set, __ATOMIC_RELAXED);
  return t >= 0 ? t : esplus_cfg.target_depth_ms;
}

static void nudge(int32_t depth, uint64_t now_ms) {
  if (esplus_lat_mode() != ESPLUS_LAT_NUDGE) return;
  if (s_tick_prev_ms) {
    int target = eff_target_ms();
    int64_t err = (int64_t)depth - target, hyst = target / 2;
    if (hyst < 10) hyst = 10;
    int64_t step = (int64_t)esplus_cfg.nudge_max_ms_per_s * (int64_t)(now_ms - s_tick_prev_ms);   /* us */
    if (err > hyst) s_corr_us += err * 1000 < step ? err * 1000 : step;
    else if (err < -hyst) s_corr_us -= -err * 1000 < step ? -err * 1000 : step;
    /* guard against a broken clock reading */
    if (s_corr_us > 1000000) s_corr_us = 1000000;
    if (s_corr_us < -1000000) s_corr_us = -1000000;
    s_corr_ms = (int32_t)((s_corr_us + (s_corr_us < 0 ? -500 : 500)) / 1000);
  }
}

static pthread_t s_smp_th;
static pthread_mutex_t s_smp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_smp_cv;
static int s_smp_run, s_smp_stop;

/* Watchdog state, under s_lock (reset per session in esplus_create_open). */
static volatile int s_error_pending;   /* set by on_error; the tick recovers, the callback must not */
static uint64_t s_stall_play, s_stall_t, s_last_recover_ms;   /* s_stall_t 0 = no sample yet */
static uint32_t s_stall_sub;
static int s_stall_armed, s_recover_count, s_have_recover, s_off_logged;
static unsigned s_vtries, s_stall_vtries;   /* video packets passed to submit_packet (any result); at the last time change */
static uint64_t s_start_t0;  /* tick time of the first tick after a video submit while not started; 0 = none */
static int s_fatal_set;
static int s_catchup_done, s_catchup_hits;   /* one-time catch-up seek; consecutive ticks above catchup_ms */

/* IDR request retry at start: the first request can be lost before the host's pipeline is up. */
static uint64_t s_idr_t0, s_idr_last;
static int s_idr_n;
#define IDR_FIRST_MS 300
#define IDR_EVERY_MS 500
#define IDR_MAX 10

/* Player rebuild state, under s_lock. s_gen counts rebuilds and closes: a queued display step of an older one is stale. */
static unsigned s_gen;
static int s_rebuilds;
static esplusplayer_video_stream_info s_vinfo;
static int s_have_vinfo;
static int s_scr_w = 1920, s_scr_h = 1080;   /* the screen size win_create_main used */
static void set_callbacks(esplusplayer_handle h);

/* The session cannot play: engine_api.c ends it with this code (once). */
static void set_fatal(int code, const char *why) {
  if (s_fatal_set) return;
  s_fatal_set = 1;
  int st = s_pl ? esplusplayer_get_state(s_pl) : -99;   /* -99 = no player (a failed rebuild) */
  mldiag_player("state", st);
  PLOG("fatal %d (%s) state=%d\n", code, why, st); MLT("fatal %d (%s) state=%d", code, why, st);
  __atomic_store_n(&esplus_stats.fatal, code, __ATOMIC_RELAXED);
}

/* Flushing seek a little behind the last video PTS, then a fresh IDR (s_need_idr). Returns -1
 * when it is too soon after the last one (an error then stays pending), else 0. Does not wait
 * for seek_done; never ends the session. */
static int recover(const char *why, uint64_t now_ms) {
  if (esplus_cfg.recover_max == 0) {
    if (!s_off_logged) { s_off_logged = 1; MLT("recovery off (%s)", why); }
    return 0;
  }
  if (esplus_stats.recovery_state == 2 || s_conflicted) return 0;
  if (s_have_recover && now_ms - s_last_recover_ms < 2000) return -1;
  if (s_recover_count >= esplus_cfg.recover_max) {
    esplus_stats.recovery_state = 2; MLT("recovery gave up"); PLOG("recovery gave up\n");
    return 0;
  }
  esplus_stats.recovery_state = 1;
  __atomic_fetch_add(&esplus_stats.recoveries, 1, __ATOMIC_RELAXED);
  s_recover_count++; s_have_recover = 1; s_last_recover_ms = now_ms;
  uint64_t target = s_last_video_pts_ms > (uint64_t)esplus_cfg.recover_back_ms
                  ? s_last_video_pts_ms - (uint64_t)esplus_cfg.recover_back_ms : 0;
  int ok = esplusplayer_seek(s_pl, target);
  PLOG("recover (%s) seek %llu ms -> %d\n", why, (unsigned long long)target, ok);
  MLT("recover (%s) seek %llu ms -> %d", why, (unsigned long long)target, ok);
  s_corr_us = 0; s_corr_ms = 0;
  s_stall_t = now_ms; s_stall_sub = esplus_stats.submitted;
  s_need_idr = 1;
  int one = 1;   /* on_conflict may store 2 meanwhile, without s_lock: keep it */
  __atomic_compare_exchange_n(&esplus_stats.recovery_state, &one, 0, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
  return 0;
}

/* Rebuild the player in two steps plus the display part. The slow player calls must not run under s_lock (the video
   thread waits on it in esplus_submit):
   1. rebuild_begin, under s_lock: counters, s_cb_live = 0, take the old player (s_pl = NULL: esplus_submit then answers
      NOT_PREPARED), reset the per-player state.
   2. rebuild_slow, no lock: stop/close the old player, create/open the new one, add the video stream.
      Then under s_lock: publish it if no close happened meanwhile (s_gen), and queue part 3.
   3. rebuild_display_main, on the ecore main loop: display, prepare_async, flags.
   esplus_close cannot run in the middle of step 2 on the sampler thread: it joins the sampler first. */
/* a short get_playing_time failure on a healthy player must not rebuild it */
#define REBUILD_NOTIME_MS 2000

struct rb_job { esplusplayer_handle old; unsigned gen; esplusplayer_video_stream_info vinfo; };

static int rebuild_allowed(void) {
  return s_rebuilds < esplus_cfg.rebuild_max && s_win && s_pl && s_have_vinfo && !s_conflicted && !s_fatal_set;
}

static void rebuild_display_main(void *data);

/* The "no playing time" rebuild needs more than max(stall_ms, 2 s), but never more than fatal_stall_ms (0 = off): the
   rebuild then runs at fatal_stall_ms, before the fatal check of the same tick. */
static uint64_t rebuild_notime_ms(void) {
  uint64_t t = esplus_cfg.stall_ms > REBUILD_NOTIME_MS ? (uint64_t)esplus_cfg.stall_ms : REBUILD_NOTIME_MS;
  if (esplus_cfg.fatal_stall_ms && t > (uint64_t)esplus_cfg.fatal_stall_ms) t = (uint64_t)esplus_cfg.fatal_stall_ms;
  return t;
}

/* under s_lock */
static void rebuild_begin(struct rb_job *j, const char *why) {
  s_rebuilds++; s_gen++;
  mldiag_rebuild();
  PLOG("player rebuild #%d (%s)\n", s_rebuilds, why); MLT("player rebuild #%d (%s)", s_rebuilds, why);
  __atomic_store_n(&s_cb_live, 0, __ATOMIC_RELEASE);   /* before the old player is stopped */
  j->old = s_pl; s_pl = NULL; j->gen = s_gen; j->vinfo = s_vinfo;
  s_prepared = s_prepare_ok = s_started = 0; s_start_tried = 0; s_error_pending = 0;
  s_stall_play = s_stall_t = 0; s_stall_sub = 0; s_stall_armed = 0; s_stall_vtries = s_vtries; s_start_t0 = 0;
  s_fps_t0 = 0; s_tick_prev_ms = 0; s_corr_us = 0; s_corr_ms = 0;
  s_catchup_done = s_catchup_hits = 0;   /* the new player builds a new backlog */
}

/* no lock held on entry */
static void rebuild_slow(struct rb_job *j) {
  int st = esplusplayer_stop(j->old), cl = esplusplayer_close(j->old);
  MLT("rebuild: stop -> %d, close -> %d", st, cl); (void)st; (void)cl;
  esplusplayer_handle np = esplusplayer_create();
  MLT("rebuild: esplusplayer_create -> %p", (void *)np);
  if (np) {
    set_callbacks(np);
    int ok = esplusplayer_open(np);
    MLT("rebuild: esplusplayer_open -> %d", ok);
    mldiag_player("rb_open", ok);
    ok = esplusplayer_add_video_stream_info(np, &j->vinfo);
    MLT("rebuild: esplusplayer_add_video_stream_info -> %d", ok);
    mldiag_player("rb_addv", ok);
  }
  pthread_mutex_lock(&s_lock);
  if (j->gen != s_gen) {   /* closed meanwhile */
    pthread_mutex_unlock(&s_lock);
    if (np) { esplusplayer_stop(np); esplusplayer_close(np); }
    return;
  }
  if (!np) { set_fatal(ML_ERR_PLAYER_NOT_STARTED, "rebuild: no player"); pthread_mutex_unlock(&s_lock); return; }   /* no tick branch runs without a player */
  s_pl = np;
  ecore_main_loop_thread_safe_call_async(rebuild_display_main, (void *)(uintptr_t)s_gen);
  pthread_mutex_unlock(&s_lock);
}

/* part 3, on the ecore main loop */
static void rebuild_display_main(void *data) {
  pthread_mutex_lock(&s_lock);
  if ((unsigned)(uintptr_t)data != s_gen || !s_pl || !s_win) {
    MLT("rebuild display: stale, skipped");
    pthread_mutex_unlock(&s_lock);
    return;
  }
  int ok = esplusplayer_set_ecore_display(s_pl, ESPLUSPLAYER_DISPLAY_TYPE_OVERLAY, s_win, 0, 0, s_scr_w, s_scr_h);
  MLT("rebuild: esplusplayer_set_ecore_display -> %d", ok);
  mldiag_player("rb_disp", ok);
  int dm = esplusplayer_set_display_mode(s_pl, ESPLUSPLAYER_DISPLAY_MODE_LETTER_BOX);
  MLT("rebuild: esplusplayer_set_display_mode -> %d", dm); (void)dm;
  __atomic_store_n(&s_cb_live, 1, __ATOMIC_RELEASE);   /* the callbacks belong to this player now */
  ok = esplusplayer_prepare_async(s_pl);
  mldiag_player("prep", ok);
  mldiag_player("rb_prep", ok);
  MLT("rebuild: prepare_async -> %d", ok);
  /* after prepare_async: an IDR submitted before it would be refused (NOT_PREPARED). The epoch makes the sink drop a
     frame that was cut before these flags. */
  s_need_idr = 1;
  s_rebase = 1;
  __atomic_fetch_add(&s_epoch, 1, __ATOMIC_RELEASE);
  pthread_mutex_unlock(&s_lock);
}

/* One sampler step; the thread calls it with the monotonic clock, the host test with a fake one. */
void esplus_sample_tick(uint64_t now_ms) {
  int want_idr = 0, do_rb = 0;
  struct rb_job job;
  pthread_mutex_lock(&s_lock);
  if (s_pl && s_started) {
    const char *why = NULL;
    uint64_t play;
    if (esplusplayer_get_playing_time(s_pl, &play)) {
      esplus_stats.playing_ms = (uint32_t)play;
      esplus_stats.depth_ms = (int32_t)(s_last_video_pts_ms - play);
      if (!s_fps_t0) { s_fps_t0 = now_ms; s_fps_sub0 = esplus_stats.submitted; }
      else if (now_ms - s_fps_t0 >= 1000) {
        esplus_stats.fps_submitted = esplus_stats.submitted - s_fps_sub0;
        s_fps_t0 = now_ms; s_fps_sub0 = esplus_stats.submitted;
      }
      if (play > 0) nudge(esplus_stats.depth_ms, now_ms);
      /* backlog from the frames submitted during prepare_async never drains: one flushing seek to the newest frame, once.
         Two ticks in a row, so a single spike does not trigger it. */
      if (esplus_cfg.catchup_ms && !s_catchup_done && !s_conflicted && play > 0) {
        s_catchup_hits = esplus_stats.depth_ms > esplus_cfg.catchup_ms ? s_catchup_hits + 1 : 0;
        if (s_catchup_hits >= 2) {
          s_catchup_done = 1;   /* even if the seek fails: never retry in a loop */
          int ok = esplusplayer_seek(s_pl, s_last_video_pts_ms);
          mldiag_catchup(esplus_stats.depth_ms, ok);
          PLOG("catchup: depth %d ms, seek %llu ms -> %d\n", esplus_stats.depth_ms, (unsigned long long)s_last_video_pts_ms, ok);
          MLT("catchup: depth %d ms, seek %llu ms -> %d", esplus_stats.depth_ms, (unsigned long long)s_last_video_pts_ms, ok);
          s_corr_us = 0; s_corr_ms = 0;
          s_stall_t = now_ms; s_stall_sub = esplus_stats.submitted; s_stall_vtries = s_vtries;
          s_need_idr = 1;
        }
      }
      s_tick_prev_ms = now_ms;
      MLT("stats depth=%dms fps=%u submitted=%u", esplus_stats.depth_ms, esplus_stats.fps_submitted, esplus_stats.submitted);
      /* stall: playing time stopped while we keep submitting. Armed after the first non-zero time. */
      if (!s_stall_t || play != s_stall_play) {
        s_stall_play = play; s_stall_t = now_ms; s_stall_sub = esplus_stats.submitted; s_stall_vtries = s_vtries;
        if (play > 0) s_stall_armed = 1;
      } else {
        if (s_stall_armed && esplus_stats.submitted != s_stall_sub && now_ms - s_stall_t > (uint64_t)esplus_cfg.stall_ms)
          why = "stall";
        /* not armed: a started player whose time stays 0 also froze (#22). recover() resets s_stall_t,
           so with recovery on this fires only after it gave up */
        if (esplus_cfg.fatal_stall_ms && s_vtries != s_stall_vtries
            && now_ms - s_stall_t > (uint64_t)esplus_cfg.fatal_stall_ms)
          set_fatal(ML_ERR_PLAYER_STALLED, "stall");
      }
    } else {   /* the time cannot be read: count it as frozen (#22) */
      if (!s_stall_t) { s_stall_t = now_ms; s_stall_vtries = s_vtries; }
      else if (rebuild_allowed() && s_vtries != s_stall_vtries && now_ms - s_stall_t > rebuild_notime_ms()) {
        rebuild_begin(&job, "no playing time"); do_rb = 1;   /* the fatal check waits: the rebuild resets its timer */
      }
      else if (esplus_cfg.fatal_stall_ms && s_vtries != s_stall_vtries
               && now_ms - s_stall_t > (uint64_t)esplus_cfg.fatal_stall_ms)
        set_fatal(ML_ERR_PLAYER_STALLED, "no playing time");
    }
    int err = __atomic_exchange_n(&s_error_pending, 0, __ATOMIC_RELAXED);
    if (err) why = "error";
    if (why && !s_conflicted && recover(why, now_ms) < 0 && err) s_error_pending = 1;   /* retry after the cooldown */
  } else if (s_pl && s_start_tried && !s_started && rebuild_allowed()) {
    rebuild_begin(&job, "start failed"); do_rb = 1;
  } else if (s_pl && esplus_cfg.start_timeout_ms && s_vtries) {
    /* never started although video was submitted: the first frame stays on screen (#22) */
    if (!s_start_t0) s_start_t0 = now_ms;
    else if (now_ms - s_start_t0 > (uint64_t)esplus_cfg.start_timeout_ms) set_fatal(ML_ERR_PLAYER_NOT_STARTED, "not started");
  }
  /* no video packet reached the player yet: ask for an IDR again (after the lock is released) */
  if (s_pl && s_prepare_called && s_vtries == 0 && s_idr_n < IDR_MAX) {
    if (!s_idr_t0) s_idr_t0 = now_ms;
    else if (now_ms - s_idr_t0 > IDR_FIRST_MS && (!s_idr_last || now_ms - s_idr_last >= IDR_EVERY_MS)) {
      s_idr_last = now_ms; s_idr_n++; want_idr = 1;
      mldiag_idr_retry(); MLT("no video yet: IDR request retry #%d", s_idr_n);
    }
  }
  s_tick_seq++;
  pthread_mutex_unlock(&s_lock);
  if (want_idr) LiRequestIdrFrame();
  if (do_rb) rebuild_slow(&job);   /* the slow player calls, without s_lock */
}

uint64_t esplus_mono_ms(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void *sampler(void *arg) {
  (void)arg;
  pthread_mutex_lock(&s_smp_mu);
  while (!s_smp_stop) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    t.tv_sec += esplus_cfg.depth_poll_ms / 1000;
    t.tv_nsec += (long)(esplus_cfg.depth_poll_ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    if (pthread_cond_timedwait(&s_smp_cv, &s_smp_mu, &t) == 0 || s_smp_stop) continue;
    pthread_mutex_unlock(&s_smp_mu);
    esplus_sample_tick(esplus_mono_ms());
    pthread_mutex_lock(&s_smp_mu);
  }
  pthread_mutex_unlock(&s_smp_mu);
  return NULL;
}

static void sampler_start(void) {
  if (s_smp_run || esplus_cfg.depth_poll_ms < 50) return;
  pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
  pthread_cond_init(&s_smp_cv, &a); pthread_condattr_destroy(&a);
  s_smp_stop = 0;
  s_smp_run = pthread_create(&s_smp_th, NULL, sampler, NULL) == 0;
  MLT("sampler start -> %d", s_smp_run);
}

static void sampler_stop(void) {   /* never call with s_lock held: the tick takes it */
  if (!s_smp_run) return;
  pthread_mutex_lock(&s_smp_mu); s_smp_stop = 1; pthread_cond_signal(&s_smp_cv); pthread_mutex_unlock(&s_smp_mu);
  pthread_join(s_smp_th, NULL);
  pthread_cond_destroy(&s_smp_cv);   /* sampler_start inits it again */
  s_smp_run = 0;
  MLT("sampler stopped");
}

static int s_ecore;   /* 1 while we hold an ecore_init()/ecore_wl2_init() reference */

static void on_prepared(int ok) { if (!__atomic_load_n(&s_cb_live, __ATOMIC_ACQUIRE)) return; s_prepare_ok = ok; s_prepared = 1; mldiag_player("pdone", ok); PLOG("prepare_async_done=%d\n", ok); MLT("cb prepare_async_done ok=%d", ok); }
static void on_ready(uint32_t st) { (void)st; MLT("cb ready_to_prepare stream=%u", st); }
/* Player callbacks may run on player threads while we hold s_lock inside a player call:
 * they must never take s_lock or call the player; atomics and flags only. */
static void on_error(uint32_t e) {
  if (!__atomic_load_n(&s_cb_live, __ATOMIC_ACQUIRE)) return;   /* a closed player */
  s_error_pending = 1; mldiag_player("err", 0);   /* own mutex, not s_lock: allowed here */
  __atomic_fetch_add(&esplus_stats.errors, 1, __ATOMIC_RELAXED);
  __atomic_store_n(&esplus_stats.last_error, e, __ATOMIC_RELAXED);
  PLOG("player error 0x%x\n", e); MLT("cb player error 0x%x", e);
}
static void on_buffer(uint32_t st, uint32_t s) {   /* counts only (no player state): not gated by s_cb_live */
  if (s == 0) __atomic_fetch_add(&esplus_stats.underruns, 1, __ATOMIC_RELAXED);
  else if (s == 1) __atomic_fetch_add(&esplus_stats.overruns, 1, __ATOMIC_RELAXED);
  if (s) PLOG("OVERRUN stream=%u\n", st);
}
static void on_seek_done(void) { MLT("cb seek_done"); }
static void on_conflict(void) {
  if (!__atomic_load_n(&s_cb_live, __ATOMIC_ACQUIRE)) return;
  s_conflicted = 1; esplus_stats.recovery_state = 2;
  PLOG("resource conflict\n"); MLT("cb resource_conflicted");
}

static void set_callbacks(esplusplayer_handle h) {
  esplusplayer_set_error_cb(h, on_error);
  esplusplayer_set_buffer_status_cb(h, on_buffer);
  esplusplayer_set_resource_conflicted_cb(h, on_conflict);
  esplusplayer_set_seek_done_cb(h, on_seek_done);
  esplusplayer_set_ready_to_prepare_cb(h, on_ready);
  esplusplayer_set_prepare_async_done_cb(h, on_prepared);
}

/* idempotent: create + open the player once. */
esplusplayer_handle esplus_create_open(void) {
  pthread_mutex_lock(&s_lock);
  if (!s_pl) {
    load_cfg();
    memset((void *)&esplus_stats, 0, sizeof esplus_stats);   /* new session */
    s_conflicted = 0; s_error_pending = 0; s_need_idr = 0; s_rebase = 0;
    s_stall_play = s_stall_t = s_last_recover_ms = 0; s_stall_sub = 0;
    s_stall_armed = s_recover_count = s_have_recover = s_off_logged = 0;
    s_prepare_called = 0; s_start_tried = 0; s_rebuilds = 0; s_have_vinfo = 0; s_idr_t0 = s_idr_last = 0; s_idr_n = 0; s_vtries = s_stall_vtries = 0; s_start_t0 = 0; s_fatal_set = 0;
    s_catchup_done = s_catchup_hits = 0;
    s_pl = esplusplayer_create();
    MLT("esplusplayer_create -> %p", (void *)s_pl);
    if (s_pl) {
      set_callbacks(s_pl);
      __atomic_store_n(&s_cb_live, 1, __ATOMIC_RELEASE);
      int ok = esplusplayer_open(s_pl);
      MLT("esplusplayer_open -> %d", ok);
      mldiag_player("open", ok);
      if (!ok) { PLOG("open failed\n"); }
      else PLOG("created+opened\n");
    } else PLOG("create failed\n");
  }
  MLT("esplus_create_open -> %p", (void *)s_pl);
  pthread_mutex_unlock(&s_lock);
  return s_pl;
}

/* Adds the video stream and keeps a copy for a player rebuild. A codec_data pointer is not kept (it may not outlive
   the call): the copy has NULL/0. Our H.264/HEVC path passes none. */
int esplus_add_video_info(const esplusplayer_video_stream_info *vi) {
  pthread_mutex_lock(&s_lock);
  s_vinfo = *vi;
  if (vi->codec_data) { MLT("esplus_add_video_info: codec_data is not cached"); s_vinfo.codec_data = NULL; s_vinfo.codec_data_length = 0; }
  s_have_vinfo = 1;
  int ok = s_pl ? esplusplayer_add_video_stream_info(s_pl, vi) : 0;
  pthread_mutex_unlock(&s_lock);
  return ok;
}

/* EFL is not thread-safe, and LiStartConnection runs video setup on the caller's
 * thread (a worker thread in the .NET host). So the Wayland display/window work runs
 * on the ecore main loop (the host's app loop; called directly when the caller is
 * already the main loop thread, as in the CLI). */
struct win_req { int w, h, rc; };

/* Stats overlay: lower the video window under the UI window. s_osd_want lives as long as the engine
   (not reset per session); s_osd_win is the last result on the current window. */
static volatile int s_osd_want;
static volatile int s_osd_win;   /* 0 not applied, 1 lowered, 2 raised, -1 symbols missing */

/* ON THE ECORE MAIN LOOP only. Never takes s_lock. */
static void *osd_apply_main(void *arg) {
  (void)arg;
  int want = s_osd_want;
  if (!s_win) { MLT("osd: no window, stored want=%d", want); return NULL; }
  if (!ecore_wl2_window_lower || !ecore_wl2_window_raise) {
    s_osd_win = -1;
    MLT("osd: missing symbol(s):%s%s", ecore_wl2_window_lower ? "" : " ecore_wl2_window_lower", ecore_wl2_window_raise ? "" : " ecore_wl2_window_raise");
  } else {
    if (want) ecore_wl2_window_lower(s_win); else ecore_wl2_window_raise(s_win);
    ecore_wl2_display_flush(s_disp);
    s_osd_win = want ? 1 : 2;
    MLT("osd: window %s", want ? "lowered" : "raised");
  }
  mldiag_osd(want, s_osd_win);
  return NULL;
}

static void *win_create_main(void *arg) {
  struct win_req *r = arg;
  MLT("win: XDG_RUNTIME_DIR=%s WAYLAND_DISPLAY=%s", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "(unset)",
      getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "(unset)");
  int n = ecore_wl2_init();
  MLT("ecore_wl2_init -> %d", n);
  if (n <= 0) { r->rc = -1; return NULL; }
  s_ecore = 1;
  s_disp = ecore_wl2_display_connect(NULL);
  MLT("ecore_wl2_display_connect -> %p", s_disp);
  if (!s_disp) { PLOG("display_connect failed (XDG_RUNTIME_DIR/WAYLAND_DISPLAY?)\n"); r->rc = -1; return NULL; }
  /* The window and the display area take the whole screen, not the stream size (#17). */
  int sw = 0, sh = 0;
  const char *src = "no symbol, fallback";
  if (ecore_wl2_display_screen_size_get) {
    if (ecore_wl2_display_sync) ecore_wl2_display_sync(s_disp);   /* right after connect no wl_output is known yet */
    ecore_wl2_display_screen_size_get(s_disp, &sw, &sh);
    src = sw > 0 && sh > 0 ? "api" : "api 0x0, fallback";
  }
  /* shortcut: 1920x1080 when the screen size is unknown (no symbol, or no output);
     upgrade if a TV with another UI plane size shows a small or cut picture. */
  if (sw <= 0 || sh <= 0) { sw = 1920; sh = 1080; }
  s_scr_w = sw; s_scr_h = sh;   /* a player rebuild uses the same area */
  MLT("win: screen %dx%d (%s; stream %dx%d)", sw, sh, src, r->w, r->h); (void)src;
  s_win = ecore_wl2_window_new(s_disp, NULL, 0, 0, sw, sh);
  MLT("ecore_wl2_window_new -> %p", s_win);
  if (!s_win) { PLOG("window_new failed\n"); r->rc = -1; return NULL; }
  ecore_wl2_window_alpha_set(s_win, 1);
  ecore_wl2_window_show(s_win);
  s_osd_win = 0;
  if (s_osd_want) osd_apply_main(NULL);   /* already on the main loop */
  else mldiag_osd(0, 0);   /* a new window is on top by default */
  ecore_wl2_display_flush(s_disp);
  MLT("window shown");
  int ok = esplusplayer_set_ecore_display(s_pl, ESPLUSPLAYER_DISPLAY_TYPE_OVERLAY, s_win, 0, 0, sw, sh);
  MLT("esplusplayer_set_ecore_display(OVERLAY) -> %d", ok);
  if (!ok) { PLOG("set_ecore_display failed\n"); r->rc = -2; return NULL; }
  int dm = esplusplayer_set_display_mode(s_pl, ESPLUSPLAYER_DISPLAY_MODE_LETTER_BOX);   /* keep the aspect ratio */
  MLT("esplusplayer_set_display_mode(LETTER_BOX) -> %d", dm); (void)dm;
  r->rc = 0;
  return NULL;
}

static void *win_destroy_main(void *arg) {
  (void)arg;
  if (s_win) { ecore_wl2_window_free(s_win); s_win = NULL; }
  s_osd_win = 0;   /* no window: nothing applied */
  if (s_disp) { ecore_wl2_display_disconnect(s_disp); s_disp = NULL; }
  if (s_ecore) { ecore_wl2_shutdown(); s_ecore = 0; }
  MLT("window destroyed");
  return NULL;
}

/* called from video setup. w/h (the stream size) are only checked and traced: the window
   and the display area take the screen size (win_create_main, #17). */
int esplus_set_window(int w, int h) {
  MLT("esplus_set_window %dx%d pl=%p", w, h, (void *)s_pl);
  if (!s_pl) { MLT("esplus_set_window -> -1 (no player)"); return -1; }
  if (w <= 0 || h <= 0) { MLT("esplus_set_window -> -1 (size)"); return -1; }
  /* ecore_init() is cheap and refcounted; in the host the app has already done it.
     It must precede the thread-safe call (the CLI has no ecore yet). */
  int n = ecore_init();
  MLT("ecore_init -> %d", n);
  if (n <= 0) { MLT("esplus_set_window -> -1 (ecore_init)"); return -1; }
  struct win_req r = { w, h, -1 };
  MLT("window: running on the main loop");
  ecore_main_loop_thread_safe_call_sync(win_create_main, &r);
  if (r.rc != 0) {
    ecore_main_loop_thread_safe_call_sync(win_destroy_main, NULL);
    ecore_shutdown();
    MLT("esplus_set_window -> -1 (%s)", r.rc == -2 ? "set_ecore_display" : "no window: a service may have no display access");
    return -1;
  }
  MLT("esplus_set_window -> 0");
  return 0;
}

/* Host API (any thread but the sampler, no locks held). The state is stored; it is applied now when a window may exist. */
int ml_engine_set_stats_osd(int on) {
  s_osd_want = on ? 1 : 0;
  MLT("ml_engine_set_stats_osd(%d)", s_osd_want);
  mldiag_osd(s_osd_want, s_osd_win);
  /* a window may exist: apply now (it also records a missing symbol for the ML-ERROR line) */
  if (__atomic_load_n(&s_ecore, __ATOMIC_ACQUIRE)) ecore_main_loop_thread_safe_call_sync(osd_apply_main, NULL);
  if (!ecore_wl2_window_lower || !ecore_wl2_window_raise) { MLT("osd: stacking symbols missing"); return -1; }
  return 0;
}
int ml_engine_get_stats_osd(void) { return s_osd_win; }

/* Host API: lock-free (s_lock is held by the sampler across player I/O); the sampler reads it at its next tick. */
int ml_engine_set_target_depth_ms(int ms) {
  if (ms < 0) return -1;
  if (ms < TARGET_MIN_MS) ms = TARGET_MIN_MS;
  if (ms > TARGET_MAX_MS) ms = TARGET_MAX_MS;
  __atomic_store_n(&s_target_set, ms, __ATOMIC_RELAXED);
  MLT("ml_engine_set_target_depth_ms(%d)", ms);
  return ms;
}
int ml_engine_get_target_depth_ms(void) {
  int t = __atomic_load_n(&s_target_set, __ATOMIC_RELAXED);
  return t >= 0 ? t : mlc_env_int("MLENGINE_TARGET_DEPTH_MS", 50);
}

/* called from video start; prepares on the first call, later calls do nothing. */
void esplus_prepare(void) {
  pthread_mutex_lock(&s_lock);
  int done = s_prepare_called; s_prepare_called = 1;
  pthread_mutex_unlock(&s_lock);
  if (done) { MLT("esplus_prepare: already done"); return; }
  int ok = esplusplayer_prepare_async(s_pl);
  MLT("esplus_prepare prepare_async -> %d", ok);
  mldiag_player("prep", ok);
  if (!ok) PLOG("prepare_async failed\n");
  sampler_start();
}

/* submit + start-once-prepared. */
int esplus_submit_ep(const esplusplayer_es_packet *pkt, unsigned epoch) {
  pthread_mutex_lock(&s_lock);
  if (epoch != s_epoch) {   /* cut before a rebuild armed the new player: old timeline, maybe no IDR */
    pthread_mutex_unlock(&s_lock);
    return ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED;
  }
  if (!s_pl) {   /* a rebuild in progress, or a failed one */
    mldiag_submit(pkt->type == ESPLUSPLAYER_STREAM_TYPE_VIDEO, 0, ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED, pkt->pts / 1000000);
    pthread_mutex_unlock(&s_lock);
    return ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED;
  }
  /* a prepare that reports ok=0 has already shown the first frame (#22): try start() anyway, once */
  if (s_prepared && !s_start_tried) {
    s_start_tried = 1; s_error_pending = 0;   /* an error before start is not a playback failure */
    int ok = esplusplayer_start(s_pl); s_started = ok ? 1 : 0;
    PLOG("start -> %d (prepare ok=%d)\n", ok, s_prepare_ok); MLT("esplus_start -> %d", ok); mldiag_player("start", ok);
  }
  int r = esplusplayer_submit_packet(s_pl, pkt);
  if (pkt->type == ESPLUSPLAYER_STREAM_TYPE_VIDEO) s_vtries++;
  mldiag_submit(pkt->type == ESPLUSPLAYER_STREAM_TYPE_VIDEO, r == ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS, r, pkt->pts / 1000000);
  if (r == ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS) {
    uint64_t ms = pkt->pts / 1000000;
    if (pkt->type == ESPLUSPLAYER_STREAM_TYPE_VIDEO) {
      s_last_video_pts_ms = ms; __atomic_fetch_add(&esplus_stats.submitted, 1, __ATOMIC_RELAXED);
    }
  }
  /* shortcut: sample per-frame trace (first 30, then every 300th, plus each change of the
     result); raise the rate here if a hang needs every frame. Logging every failure filled
     the console buffer and pushed the session start out of the log; failures are counted
     in the ML-ERROR line. Counters are under s_lock. */
  static unsigned n; static int last_r = ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS; n++;
  if (n <= 30 || n % 300 == 0 || r != last_r) MLT("esplus_submit #%u -> %d", n, r);
  last_r = r;
  pthread_mutex_unlock(&s_lock);
  return r;
}

int esplus_submit(const esplusplayer_es_packet *pkt) { return esplus_submit_ep(pkt, esplus_epoch()); }

void esplus_close(void) {
  MLT("esplus_close");
  sampler_stop();   /* joins the sampler: a rebuild on the sampler thread (rebuild_slow) cannot run in the middle of the close */
  pthread_mutex_lock(&s_lock);
  __atomic_store_n(&s_cb_live, 0, __ATOMIC_RELEASE);
  if (s_pl) {
    int st = esplusplayer_stop(s_pl), cl = esplusplayer_close(s_pl); (void)st; (void)cl;
    MLT("esplusplayer_stop -> %d, close -> %d", st, cl);
    s_pl = NULL;
  }
  s_gen++; s_rebuilds = 0; s_have_vinfo = 0;   /* a queued rebuild display step is now stale */
  s_prepared = s_prepare_ok = s_started = 0; s_start_tried = 0; s_prepare_called = 0;
  s_last_video_pts_ms = 0; s_fps_t0 = 0;
  s_corr_us = 0; s_corr_ms = 0; s_tick_prev_ms = 0;
  s_catchup_done = s_catchup_hits = 0;
  pthread_mutex_unlock(&s_lock);
  /* Release only what esplus_set_window took: an unbalanced ecore_shutdown() would
     shut down the host app's own ecore loop. */
  if (s_win || s_disp || s_ecore) {
    ecore_main_loop_thread_safe_call_sync(win_destroy_main, NULL);
    ecore_shutdown();
  }
}

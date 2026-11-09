/*
 * esplus_stats.h - engine-internal contract between esplus_player.c, video_esplus.c
 * and engine_api.c (the public ABI stays mlengine.h). The counters work in release
 * builds: they are a product feature (UI overlay), not trace. Update them with
 * __atomic_fetch_add(..., __ATOMIC_RELAXED); player callbacks must never take the
 * player lock.
 */
#ifndef ESPLUS_STATS_H
#define ESPLUS_STATS_H
#include <stdint.h>

typedef struct {
  volatile uint32_t submitted;      /* video packets accepted (SUCCESS) */
  volatile uint32_t dropped_late;   /* skipped by the in-flight cap */
  volatile uint32_t dropped_full;   /* lost after FULL retries */
  volatile uint32_t underruns, overruns;
  volatile uint32_t errors, last_error;
  volatile uint32_t recoveries;
  volatile int      recovery_state; /* 0 idle, 1 recovering, 2 gave up / resource conflict */
  volatile int32_t  depth_ms;       /* video: last pts - playing_time */
  volatile int32_t  audio_depth_ms;
  volatile uint32_t playing_ms;
  volatile uint32_t fps_submitted;  /* submits in the last full second */
  volatile int      fatal;          /* 0, or an ML_ERR_PLAYER_* code: the engine ends the session */
  /* native audio output (audio_opus.c; update with __atomic in audio_opus.c) */
  volatile int32_t  aout_route;     /* 0 none, 1 callback (web audio), 2 native */
  volatile uint32_t aout_frames;    /* frames written to the ring */
  volatile uint32_t aout_underruns, aout_overruns, aout_drops;   /* native path counters */
  volatile int32_t  aout_buf_ms;    /* ring fill in ms (last report) */
} esplus_stats_t;

enum { ESPLUS_LAT_OFF, ESPLUS_LAT_NUDGE, ESPLUS_LAT_ARRIVAL };
typedef struct { int latency_mode, target_depth_ms, nudge_max_ms_per_s, drop_after_ms,
                 depth_poll_ms, stall_ms, recover_max, recover_back_ms,
                 start_timeout_ms, fatal_stall_ms, rebuild_max, catchup_ms; } esplus_cfg_t;   /* the last four: 0 = off */

/* engine-local fatal codes (moonlight-common-c uses -100..-104) */
#define ML_ERR_PLAYER_NOT_STARTED -110   /* no start() success, video submitted for start_timeout_ms */
#define ML_ERR_PLAYER_STALLED     -111   /* playing time frozen for fatal_stall_ms while submitting */

extern esplus_stats_t esplus_stats;   /* counters: update with __atomic_fetch_add(..., __ATOMIC_RELAXED) */
extern esplus_cfg_t esplus_cfg;       /* read once per session in esplus_create_open, read-only afterwards; the runtime target override lives in esplus_player.c */
/* Effective latency mode: esplus_cfg.latency_mode, but nudge when a runtime target is set and the env mode is off. */
int esplus_lat_mode(void);

/* One sampler step (playing time, depth, fps). The sampler thread calls it every
 * depth_poll_ms; the host test calls it with a fake clock. */
void esplus_sample_tick(uint64_t now_ms);

/* Latency nudge (mode nudge): ms the video sink subtracts from each PTS. Lock-free. */
int32_t esplus_correction_ms(void);
/* Counts sampler ticks; video uses it to skip at most one late frame per tick. */
uint32_t esplus_sample_seq(void);
/* 1 once after a recovery seek (then 0): the video sink must return DR_NEED_IDR. */
int esplus_take_need_idr(void);
/* 1 once after a player rebuild (then 0): the video sink restarts its PTS at 0. */
int esplus_take_rebase(void);
/* Rebuild epoch (atomic read). The video sink reads it before esplus_take_*() and submits with esplus_submit_ep: a submit
 * with an older epoch is refused (NOT_PREPARED) so a frame cut for the old player never reaches the new one. */
unsigned esplus_epoch(void);
/* CLOCK_MONOTONIC in ms (sampler, arrival-mode PTS). */
uint64_t esplus_mono_ms(void);

#endif

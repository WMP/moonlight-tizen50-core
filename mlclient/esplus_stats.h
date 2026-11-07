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
  volatile uint32_t audio_written_ms;   /* audio the PulseAudio writer has written (audio_pulse.c, __atomic); read for the A/V drift check */
} esplus_stats_t;

enum { ESPLUS_LAT_OFF, ESPLUS_LAT_NUDGE, ESPLUS_LAT_ARRIVAL };
typedef struct { int latency_mode, target_depth_ms, nudge_max_ms_per_s, drop_after_ms,
                 depth_poll_ms, stall_ms, recover_max, recover_back_ms,
                 start_timeout_ms, fatal_stall_ms; } esplus_cfg_t;   /* the last two: 0 = off */

/* engine-local fatal codes (moonlight-common-c uses -100..-104) */
#define ML_ERR_PLAYER_NOT_STARTED -110   /* no start() success, video submitted for start_timeout_ms */
#define ML_ERR_PLAYER_STALLED     -111   /* playing time frozen for fatal_stall_ms while submitting */

extern esplus_stats_t esplus_stats;   /* counters: update with __atomic_fetch_add(..., __ATOMIC_RELAXED) */
extern esplus_cfg_t esplus_cfg;       /* read once per session in esplus_create_open, read-only afterwards */

/* One sampler step (playing time, depth, fps). The sampler thread calls it every
 * depth_poll_ms; the host test calls it with a fake clock. */
void esplus_sample_tick(uint64_t now_ms);

/* Latency nudge (mode nudge): ms the video sink subtracts from each PTS. Lock-free. */
int32_t esplus_correction_ms(void);
/* Counts sampler ticks; video uses it to skip at most one late frame per tick. */
uint32_t esplus_sample_seq(void);
/* 1 once after a recovery seek (then 0): the video sink must return DR_NEED_IDR. */
int esplus_take_need_idr(void);
/* CLOCK_MONOTONIC in ms (sampler, arrival-mode PTS). */
uint64_t esplus_mono_ms(void);

#endif

/*
 * mldiag.h - engine-internal session diagnostics (not part of the ABI, mlengine.h).
 * Collects counters and a few moonlight-common-c log lines in release builds too, and
 * on a failed session builds ONE "ML-ERROR ..." line. Pure C, no engine headers.
 * Holds no host address, key, PIN, URL or certificate (the host is masked as <host> in the cc lines).
 */
#ifndef MLDIAG_H
#define MLDIAG_H

typedef struct {
  int width, height, fps, bitrate_kbps, packet_size, supported_video_formats, color_space,
      color_range, audio_config, server_codec_mode_support, encryption_flags;
  const char *app_version, *gfe_version;   /* copied; may be NULL */
  const unsigned char *aes_key;            /* 16 bytes, compare only (never stored or printed); NULL = unknown */
  int rikeyid;                             /* compare only */
  const char *rtsp_scheme;                 /* "rtsp" or "rtspenc" (copied; no secret); NULL = unknown */
  const char *host;                        /* copied, only to mask it in the cc lines (never printed); NULL = none */
} mldiag_cfg;

void mldiag_reset(const mldiag_cfg *cfg);                 /* session start: clears everything */
void mldiag_cc_line(const char *line);                    /* one moonlight-common-c log line */
void mldiag_video_setup(int fmt, int w, int h, int fps, int rc);
void mldiag_video_du(int is_idr);                         /* every decode unit */
void mldiag_submit(int is_video, int ok, int status, unsigned long long pts_ms);   /* esplus_submit result: ok != 0 = success, else the player status (0 = NOT_PREPARED); pts_ms of the packet */
void mldiag_player(const char *what, int rc);             /* "open", "win", "prep", "pdone" (prepare_async_done), "start", "rb_open"/"rb_addv"/"rb_disp"/"rb_prep" (the player rebuild steps; -99 = not reached), "state" (raw esplusplayer_get_state at a fatal stop) or "err" (a player error: only its first time is kept) */
/* /launch or /resume URL about to be sent: remembers the mode, rikey and rikeyid (not printed). The next
   mldiag_reset(cfg) compares them with the session's key and forgets them. Survives mldiag_reset. */
void mldiag_note_launch(const char *url);
/* One control-stream AES-GCM encrypt: ok = real result, ivlen, verify: 1 decrypt check ok, 0 failed, -1 not done. */
unsigned mldiag_ctl_encrypt(int ok, int ivlen, int verify);   /* returns this session's message number (1, 2, ...) */
/* One pad call (UI gamepad input): arrival != 0 = arrival else state; rc != 0 = failure (counted, last rc kept). */
void mldiag_pad(int arrival, int rc);
/* Audio: one libopus decode: ok != 0 = decoded packet, else a failure (err = opus code, last kept). */
void mldiag_adec(int ok, int err);
/* Audio output (audio_opus.c): which path is active. 0 = not decided yet, 1 = the host audio callback
   (web UI / Web Audio), 2 = none (no callback), 3 = native audio-io (audio_out_*), 4 = native failed,
   fell back to the callback. One frame handed to the output (bytes of PCM).
   The host's dropped audio frames of this session (ml_engine_set_audio_drops; last report kept).
   Native counters (path 3/4): written frames, underruns, overruns, dropped bytes (latency cap) and
   the ring-buffer fill in ms at read time. */
void mldiag_aout(int has_cb);
void mldiag_aout_native(int active);   /* audio_opus.c: native output came up (1) or fell back (0) */
void mldiag_aout_reason(const char *reason);   /* audio_opus.c: why native output is not used ("" = none yet) */
void mldiag_anative(unsigned frames, unsigned underruns, unsigned overruns, unsigned drops, int buf_ms);
void mldiag_aframe(int bytes);
void mldiag_adrops(int dropped);
/* The host's audio counters (ml_engine_set_audio_host_stats): sent to the UI, dropped by its queue, failed sends. mldiag_last shows the values at read time. */
void mldiag_ahost(int sent, int dropped, int send_failures);
void mldiag_rebuild(void);            /* one player rebuild (esplus_player.c) */
void mldiag_catchup(int depth_ms, int seek_ok);   /* the one-time catch-up seek (esplus_player.c): depth before, seek result */
void mldiag_osd(int want, int win);   /* stats overlay (esplus_player.c): want persists across sessions; win 0 none, 1 lowered, 2 raised, -1 no symbols */
void mldiag_idr_retry(void);          /* one IDR request retry at the start (no video packet yet) */
/* Build, store and print the line. extra values come from the caller; enc_* are the
   negotiated encryption features (supported by the host / requested by it / enabled). */
void mldiag_fail(int code, int stage, const char *stage_name, int host_feature_flags,
                 int rtt, int rtt_var, unsigned player_errors, unsigned player_last_error,
                 unsigned enc_supported, unsigned enc_requested, unsigned enc_enabled);
/* Returns the stored code (0 = none); copies the line (NUL-terminated, truncated) if buf && len > 0. */
int mldiag_last(char *buf, int len);

#endif

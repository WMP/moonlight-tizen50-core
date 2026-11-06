/*
 * mldiag.h - engine-internal session diagnostics (not part of the ABI, mlengine.h).
 * Collects counters and a few moonlight-common-c log lines in release builds too, and
 * on a failed session builds ONE "ML-ERROR ..." line. Pure C, no engine headers.
 * Holds no host address, key, PIN, URL or certificate.
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
} mldiag_cfg;

void mldiag_reset(const mldiag_cfg *cfg);                 /* session start: clears everything */
void mldiag_cc_line(const char *line);                    /* one moonlight-common-c log line */
void mldiag_video_setup(int fmt, int w, int h, int fps, int rc);
void mldiag_video_du(int is_idr);                         /* every decode unit */
void mldiag_submit(int is_video, int ok, int status, unsigned long long pts_ms);   /* esplus_submit result: ok != 0 = success, else the player status (0 = NOT_PREPARED); pts_ms of the packet */
void mldiag_player(const char *what, int rc);             /* "open", "win", "prep", "pdone" (prepare_async_done), "start", "state" (raw esplusplayer_get_state at a fatal stop) or "err" (a player error: only its first time is kept) */
/* /launch or /resume URL about to be sent: remembers the mode, rikey and rikeyid (not printed). The next
   mldiag_reset(cfg) compares them with the session's key and forgets them. Survives mldiag_reset. */
void mldiag_note_launch(const char *url);
/* One control-stream AES-GCM encrypt: ok = real result, ivlen, verify: 1 decrypt check ok, 0 failed, -1 not done. */
unsigned mldiag_ctl_encrypt(int ok, int ivlen, int verify);   /* returns this session's message number (1, 2, ...) */
/* One pad call (UI gamepad input): arrival != 0 = arrival else state; rc != 0 = failure (counted, last rc kept). */
void mldiag_pad(int arrival, int rc);
/* Audio: the mime of the stream info (-99 until set); one libopus decode: ok != 0 = decoded packet, else a failure (err = opus code, last kept). */
void mldiag_audio_mime(int mime);
void mldiag_adec(int ok, int err);
/* Build, store and print the line. extra values come from the caller; enc_* are the
   negotiated encryption features (supported by the host / requested by it / enabled). */
void mldiag_fail(int code, int stage, const char *stage_name, int host_feature_flags,
                 int rtt, int rtt_var, unsigned player_errors, unsigned player_last_error,
                 unsigned enc_supported, unsigned enc_requested, unsigned enc_enabled);
/* Returns the stored code (0 = none); copies the line (NUL-terminated, truncated) if buf && len > 0. */
int mldiag_last(char *buf, int len);

#endif

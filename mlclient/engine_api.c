/*
 * engine_api.c - implements the libmlengine.so C ABI (mlengine.h).
 *
 * This is the former body of main.c, turned into a reusable, non-blocking engine
 * so the .NET host and the CLI harness can both drive it.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "Limelight.h"
#include "errors.h"
#include "esplus_stats.h"
#include "mlengine.h"
#include "mldiag.h"
#include "mltrace.h"

extern int  ml_cert_init(const char *dir);
extern void VidEsplus_FillCallbacks(PDECODER_RENDERER_CALLBACKS cb);
extern void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb);  /* audio_opus.c */
extern int  input_init(void);                                      /* input_evdev.c */
extern void input_shutdown(void);
extern int  gs_orchestrate(const char *host, unsigned short httpPort, unsigned short httpsPort,
                           int appId, const char *pin, STREAM_CONFIGURATION *sc,
                           SERVER_INFORMATION *si, const char *datadir);

static volatile int g_running;   /* 1 while a session is up */
static volatile int g_started;   /* 1 once LiStartConnection succeeded (needs LiStop) */
static char g_host[256];         /* si.address is held by moonlight-common-c for the
                                    session, so the host string must outlive the call */

void (*volatile ml_log_cb)(const char *line);   /* see mltrace.h */

void ml_engine_set_log_cb(ml_engine_log_cb cb) {
  ml_log_cb = cb;
  MLT("log forwarding %s", cb ? "on" : "off");
}

static STREAM_CONFIGURATION g_sc;  /* static: held by moonlight-common-c for the session */
static SERVER_INFORMATION g_si;

/* moonlight-common-c session globals (Connection.c), set by the RTSP handshake. */
extern uint32_t EncryptionFeaturesSupported, EncryptionFeaturesRequested, EncryptionFeaturesEnabled;

/* Build the one-line failure report (also in release) from the engine counters. */
static void report_failure(int code, int stage) {
  uint32_t rtt = 0, var = 0;
  if (!LiGetEstimatedRttInfo(&rtt, &var)) rtt = var = 0;
  mldiag_fail(code, stage, stage >= 0 ? LiGetStageName(stage) : NULL, (int)LiGetHostFeatureFlags(),
              (int)rtt, (int)var, esplus_stats.errors, esplus_stats.last_error,
              EncryptionFeaturesSupported, EncryptionFeaturesRequested, EncryptionFeaturesEnabled);
  char line[2048]; mldiag_last(line, sizeof line);
  MLT("%s", line);
}

static void cl_stageFailed(int stage, int errorCode) {
  fprintf(stderr, "[ml] stage %d FAILED err=%d\n", stage, errorCode);
  MLT("stageFailed stage=%d err=%d", stage, errorCode);
  report_failure(errorCode, stage);
}
/* Host dropped the stream -> clear running so ml_engine_wait() returns and the
   caller runs ml_engine_stop(), which releases the HW overlay. Without this the
   overlay stays grabbed and blocks the TV's own broadcast video. */
static void cl_connTerminated(int errorCode) {
  fprintf(stderr, "[ml] connection terminated err=%d\n", errorCode);
  MLT("connectionTerminated err=%d", errorCode);
  if (errorCode != 0) report_failure(errorCode, -1);   /* before g_running = 0: the host reads it after */
  g_running = 0;
}
/* moonlight-common-c is built with LC_LOG (build-deps-lib.sh): Limelog formats the line
   itself and passes it as the only argument, so it is plain text here; a '%' in it must
   not be expanded again. */
static void cl_logMessage(const char *format, ...) {
  fputs(format, stderr);
  mldiag_cc_line(format);
  MLT("cc: %s", format);
}

static void cl_stageStarting(int stage) { (void)stage; MLT("stage %d (%s) starting", stage, LiGetStageName(stage)); }
static void cl_stageComplete(int stage) { (void)stage; MLT("stage %d (%s) complete", stage, LiGetStageName(stage)); }

/* Shared tail of both start paths: callbacks, LiStartConnection, input.
   Uses g_sc/g_si. Returns 0 or 1 (LiStartConnection failed). */
static int start_connection(void) {
  DECODER_RENDERER_CALLBACKS dr; VidEsplus_FillCallbacks(&dr);
  AUDIO_RENDERER_CALLBACKS   ar; AudEsplus_FillCallbacks(&ar);
  CONNECTION_LISTENER_CALLBACKS cl;
  LiInitializeConnectionCallbacks(&cl);
  cl.stageStarting = cl_stageStarting;
  cl.stageComplete = cl_stageComplete;
  cl.stageFailed = cl_stageFailed;
  cl.connectionTerminated = cl_connTerminated;
  cl.logMessage = cl_logMessage;

  MLT("LiStartConnection %dx%d@%d %dkbps fmt=0x%x audio=0x%x pkt=%d", g_sc.width, g_sc.height, g_sc.fps,
      g_sc.bitrate, g_sc.supportedVideoFormats, g_sc.audioConfiguration, g_sc.packetSize);
  fprintf(stderr, "[ml] LiStartConnection %dx%d@%d %dkbps -> %s\n",
          g_sc.width, g_sc.height, g_sc.fps, g_sc.bitrate, (const char *)g_si.address);
  char rtspscheme[12] = "-";   /* "rtsp" or "rtspenc": which RTSP transport the host gave us (no secret) */
  if (g_si.rtspSessionUrl) {
    const char *sep = strstr(g_si.rtspSessionUrl, "://");
    int n = sep ? (int)(sep - g_si.rtspSessionUrl) : 0;
    if (n > 0 && n < (int)sizeof rtspscheme) { memcpy(rtspscheme, g_si.rtspSessionUrl, (size_t)n); rtspscheme[n] = 0; }
  }
  mldiag_cfg dc = { g_sc.width, g_sc.height, g_sc.fps, g_sc.bitrate, g_sc.packetSize,
                    g_sc.supportedVideoFormats, g_sc.colorSpace, g_sc.colorRange, g_sc.audioConfiguration,
                    g_si.serverCodecModeSupport, g_sc.encryptionFlags,
                    g_si.serverInfoAppVersion, g_si.serverInfoGfeVersion,
                    (const unsigned char *)g_sc.remoteInputAesKey, 0, rtspscheme };
  { unsigned id; memcpy(&id, g_sc.remoteInputAesIv, 4); dc.rikeyid = (int)ntohl(id); }   /* rikeyid is the IV's first 4 bytes, big-endian */
  mldiag_reset(&dc);
  int err = LiStartConnection(&g_si, &g_sc, &cl, &dr, &ar, NULL, 0, NULL, 0);
  if (err != 0) { fprintf(stderr, "[ml] LiStartConnection failed: %d\n", err); MLT("start_connection -> 1 (LiStartConnection %d)", err); return 1; }
  MLT("LiStartConnection -> 0");
  g_started = 1; g_running = 1;

  int in = input_init();
  MLT("input_init -> %d", in);
  if (in != 0) fprintf(stderr, "[ml] input disabled (no evdev access)\n");
  fprintf(stderr, "[ml] streaming\n");
  return 0;
}

int ml_engine_start(const char *host, int appId, const char *pin,
                    int width, int height, int fps, int bitrate_kbps,
                    const char *datadir) {
  MLT("ml_engine_start host=%s appId=%d pin=%s %dx%d@%d %dkbps datadir=%s",
      host ? host : "(null)", appId, pin ? "(set)" : "(null)", width, height, fps,
      bitrate_kbps, datadir ? datadir : "(null)");
  if (g_running) { MLT("ml_engine_start -> 0 (already streaming)"); return 0; }
  if (g_started) ml_engine_stop();  /* previous session ended by host drop but was never stopped */
  mldiag_reset(NULL);   /* an early failure must not report the previous session */
  if (!host) { MLT("ml_engine_start -> 2 (no host)"); return 2; }
  if (!datadir) datadir = "/tmp";
  if (!width)  width = 1920;
  if (!height) height = 1080;
  if (!fps)    fps = 60;
  if (!bitrate_kbps) bitrate_kbps = 20000;
  snprintf(g_host, sizeof g_host, "%s", host);
  const unsigned short httpPort = 47989, httpsPort = 47984;

  int certrc = ml_cert_init(datadir);
  MLT("cert init rc=%d", certrc);
  if (certrc != 0) { fprintf(stderr, "[ml] cert init failed\n"); MLT("ml_engine_start -> 1 (cert)"); return 1; }

  LiInitializeStreamConfiguration(&g_sc);
  g_sc.width = width; g_sc.height = height; g_sc.fps = fps; g_sc.bitrate = bitrate_kbps;
  g_sc.packetSize = 1392;   /* LAN packet size used by GameStream clients (fewer packets, less FEC overhead at high bitrates) */
  g_sc.supportedVideoFormats = VIDEO_FORMAT_H264;   /* HEVC later */
  g_sc.colorSpace = COLORSPACE_REC_709;
  g_sc.colorRange = COLOR_RANGE_LIMITED;
  g_sc.audioConfiguration = AUDIO_CONFIGURATION_STEREO;

  LiInitializeServerInformation(&g_si);
  g_si.address = g_host;

  if (gs_orchestrate(g_host, httpPort, httpsPort, appId, pin, &g_sc, &g_si, datadir) != 0) {
    fprintf(stderr, "[ml] GameStream orchestration failed; cannot start stream.\n");
    MLT("ml_engine_start -> 3 (orchestrate)");
    return 3;
  }
  g_si.serverCodecModeSupport = SCM_H264;

  if (start_connection() != 0) return 1;
  MLT("ml_engine_start -> 0");
  return 0;
}

int ml_engine_get_stats(ml_engine_stats *out) {
  if (!out || out->struct_size < (int32_t)sizeof *out) return GS_INVALID;
  const esplus_stats_t *s = &esplus_stats;
  out->running = g_running;
  out->depth_ms = s->depth_ms; out->audio_depth_ms = s->audio_depth_ms; out->playing_ms = s->playing_ms;
  out->submitted = s->submitted; out->dropped_late = s->dropped_late; out->dropped_full = s->dropped_full;
  out->fps_submitted = s->fps_submitted; out->underruns = s->underruns; out->overruns = s->overruns;
  out->errors = s->errors; out->last_error = s->last_error;
  out->recoveries = s->recoveries; out->recovery_state = s->recovery_state;
  return 0;
}

int ml_engine_last_error(char *buf, int len) { return mldiag_last(buf, len); }

/* The player watchdog set a fatal code: report it and end the session (the host then reads the line). */
static void check_player_fatal(void) {
  if (!g_running) return;
  int f = __atomic_exchange_n(&esplus_stats.fatal, 0, __ATOMIC_RELAXED);
  if (!f) return;
  MLT("player fatal %d", f);
  report_failure(f, -1);
  g_running = 0;
}

int ml_engine_running(void) {   /* polled every 500 ms by the host: trace only changes */
  check_player_fatal();
  static int last = -1; int r = g_running;
  if (r != last) { MLT("ml_engine_running -> %d", r); last = r; }
  return r;
}

void ml_engine_wait(void) {
  MLT("ml_engine_wait");
  while (g_running) { check_player_fatal(); struct timespec ts = {0, 200000000}; nanosleep(&ts, NULL); }
  MLT("ml_engine_wait -> returned");
}

void ml_engine_stop(void) {
  MLT("ml_engine_stop started=%d", g_started);
  g_running = 0;
  if (!g_started) { MLT("ml_engine_stop -> nothing to stop"); return; }
  g_started = 0;
  fprintf(stderr, "[ml] stopping, releasing overlay\n");
  input_shutdown();
  LiStopConnection();   /* tears down stages -> video sink cleanup -> esplus_close */
  MLT("ml_engine_stop -> done");
}

/* Copy for the session lifetime: moonlight-common-c keeps these pointers. */
static char g_sess_host[256], g_sess_appver[64], g_sess_gfever[64], g_sess_rtsp[512];

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int start_session_impl(const char *host, int width, int height, int fps, int bitrate_kbps,
                              const char *rikey_hex, int rikeyid, const char *app_version,
                              const char *gfe_version, const char *rtsp_url,
                              int server_codec_mode_support, const char *video_codec, int hdr,
                              const char *audio_config, int full_range) {
  MLT("ml_engine_start_session host=%s %dx%d@%d %dkbps rikey=%s rikeyid=%d appver=%s gfever=%s rtsp=%s scm=%d codec=%s hdr=%d audio=%s full_range=%d",
      host ? host : "(null)", width, height, fps, bitrate_kbps, rikey_hex ? "(set)" : "(null)",
      rikeyid, app_version ? app_version : "(null)", gfe_version ? gfe_version : "(null)",
      rtsp_url ? "(set)" : "(null)", server_codec_mode_support, video_codec ? video_codec : "(null)",
      hdr, audio_config ? audio_config : "(null)", full_range);
  if (g_running) { MLT("ml_engine_start_session -> 0 (already streaming)"); return 0; }
  if (g_started) ml_engine_stop();
  mldiag_reset(NULL);   /* an early failure must not report the previous session */
  if (!host || !rikey_hex || strlen(rikey_hex) != 32) { MLT("ml_engine_start_session -> GS_INVALID"); return GS_INVALID; }
  unsigned char key[16];
  for (int i = 0; i < 16; i++) {
    int hi = hexval(rikey_hex[i * 2]), lo = hexval(rikey_hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) { MLT("ml_engine_start_session -> GS_INVALID (rikey hex)"); return GS_INVALID; }
    key[i] = (unsigned char)(hi << 4 | lo);
  }
  if (!width)  width = 1920;
  if (!height) height = 1080;
  if (!fps)    fps = 60;
  if (!bitrate_kbps) bitrate_kbps = 20000;
  /* validate at the ABI boundary: bad values return an error the host can report */
  if (width < 320 || width > 7680 || height < 240 || height > 4320 || fps < 10 || fps > 240 ||
      bitrate_kbps < 500 || bitrate_kbps > 500000 || !*host) {
    MLT("ml_engine_start_session -> GS_INVALID (bad stream settings)");
    return GS_INVALID;
  }
  snprintf(g_sess_host, sizeof g_sess_host, "%s", host);
  snprintf(g_sess_appver, sizeof g_sess_appver, "%s", app_version ? app_version : "");
  snprintf(g_sess_gfever, sizeof g_sess_gfever, "%s", gfe_version ? gfe_version : "");
  snprintf(g_sess_rtsp, sizeof g_sess_rtsp, "%s", rtsp_url ? rtsp_url : "");

  LiInitializeStreamConfiguration(&g_sc);
  g_sc.width = width; g_sc.height = height; g_sc.fps = fps; g_sc.bitrate = bitrate_kbps;
  g_sc.packetSize = 1392;   /* LAN packet size used by GameStream clients (fewer packets, less FEC overhead at high bitrates) */
  if (video_codec && !strcmp(video_codec, "HEVC"))
    g_sc.supportedVideoFormats = hdr ? VIDEO_FORMAT_H265_MAIN10 : VIDEO_FORMAT_H265;
  else {
    if (video_codec && !strcmp(video_codec, "AV1")) {
      fprintf(stderr, "[ml] AV1 not supported by the video sink, using H264\n");
      MLT("start_session: AV1 -> H264 fallback");
    }
    g_sc.supportedVideoFormats = VIDEO_FORMAT_H264;
  }
  int hevc = g_sc.supportedVideoFormats & VIDEO_FORMAT_MASK_H265;
  g_sc.colorSpace = (hdr && hevc) ? COLORSPACE_REC_2020 : COLORSPACE_REC_709;
  g_sc.colorRange = full_range ? COLOR_RANGE_FULL : COLOR_RANGE_LIMITED;
  MLT("start_session: colorSpace=%d colorRange=%d", g_sc.colorSpace, g_sc.colorRange);
  g_sc.audioConfiguration = !audio_config ? AUDIO_CONFIGURATION_STEREO
    : !strcmp(audio_config, "51Surround") ? AUDIO_CONFIGURATION_51_SURROUND
    : !strcmp(audio_config, "71Surround") ? AUDIO_CONFIGURATION_71_SURROUND
    : AUDIO_CONFIGURATION_STEREO;
  memcpy(g_sc.remoteInputAesKey, key, 16);
  memset(g_sc.remoteInputAesIv, 0, 16);
  int iv = (int)htonl((unsigned int)rikeyid);
  memcpy(g_sc.remoteInputAesIv, &iv, sizeof iv);

  LiInitializeServerInformation(&g_si);
  g_si.address = g_sess_host;
  g_si.serverInfoAppVersion = g_sess_appver;
  g_si.serverInfoGfeVersion = g_sess_gfever;
  g_si.rtspSessionUrl = g_sess_rtsp[0] ? g_sess_rtsp : NULL;
  g_si.serverCodecModeSupport = server_codec_mode_support ? server_codec_mode_support : SCM_H264;

  int rc = start_connection();
  MLT("ml_engine_start_session -> %d", rc);
  return rc;
}

int ml_engine_start_session(const char *host, int width, int height, int fps, int bitrate_kbps,
                            const char *rikey_hex, int rikeyid, const char *app_version,
                            const char *gfe_version, const char *rtsp_url,
                            int server_codec_mode_support, const char *video_codec, int hdr,
                            const char *audio_config) {
  return start_session_impl(host, width, height, fps, bitrate_kbps, rikey_hex, rikeyid, app_version,
                            gfe_version, rtsp_url, server_codec_mode_support, video_codec, hdr,
                            audio_config, 0);
}

int ml_engine_start_session_v2(const char *host, int width, int height, int fps, int bitrate_kbps,
                               const char *rikey_hex, int rikeyid, const char *app_version,
                               const char *gfe_version, const char *rtsp_url,
                               int server_codec_mode_support, const char *video_codec, int hdr,
                               const char *audio_config, int full_range) {
  return start_session_impl(host, width, height, fps, bitrate_kbps, rikey_hex, rikeyid, app_version,
                            gfe_version, rtsp_url, server_codec_mode_support, video_codec, hdr,
                            audio_config, full_range);
}

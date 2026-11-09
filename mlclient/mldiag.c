/* mldiag.c - see mldiag.h. Host self-test: tests/mldiag_test.c. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "mldiag.h"

#define LINE_MAX_CC 160
#define FIRST_N 4
#define LAST_N 8
#define OUT_MAX 2048

/* weak: some tests link mldiag.c without esplus_player.c; lock-free, so it is safe under s_mu */
int ml_engine_get_target_depth_ms(void) __attribute__((weak));

static pthread_mutex_t s_mu = PTHREAD_MUTEX_INITIALIZER;
static mldiag_cfg s_cfg;
static char s_appver[64], s_gfever[64], s_rtsp[12] = "-", s_host[256];   /* s_host: only to mask the cc lines */
static long long s_t0;
static char s_first[FIRST_N][LINE_MAX_CC + 1], s_last[LAST_N][LINE_MAX_CC + 1];
static int s_nlines;                      /* total cc lines seen */
static char s_out[OUT_MAX]; static int s_code;
static int s_vfmt, s_vw, s_vh, s_vfps, s_vrc, s_vset;
static int s_rb_open = -99, s_rb_addv = -99, s_rb_disp = -99, s_rb_prep = -99;   /* player rebuild steps */
static int s_open = -99, s_win = -99, s_prep = -99, s_pdone = -99, s_start = -99, s_state = -99;   /* -99 = not reached */
static int s_cu_n, s_cu_depth = -1, s_cu_rc = -99;   /* catch-up seek: count, depth before, result; -1/-99 = not reached */
static int s_osd_want, s_osd_win;   /* stats overlay: want survives mldiag_reset (engine lifetime), win result is per session */
static int s_first_pkt_ms = -1, s_sub_last_bad, s_vfail_last, s_afail_last;
/* absolute now_ms() of the player events, -1 = not reached; printed relative to s_t_prep. Under s_mu (the hot path
   reads s_t_v1/s_t_a1 atomically and locks only for the first success). */
static long long s_t_prep = -1, s_t_pdone = -1, s_t_err = -1, s_t_v1 = -1, s_t_a1 = -1, s_vpts1 = -1, s_apts1 = -1;
static long long s_first_du_ms = -1;

/* hot counters: __atomic */
enum { C_UNREC, C_DROP, C_WAITIDR, C_BADLEN, C_DECRYPT, C_CORRUPT, C_UNKFT, C_IDRREQ, C_FEC, C_QOVF,
       C_DROPLIM, C_DU, C_IDR, C_VSUB, C_ASUB, C_SUBFAIL, C_CTLENC, C_CTLFAIL, C_VOK, C_VFAIL, C_VNA, C_PADARR, C_PADST, C_PADFAIL, C_VSUBFAIL, C_ASUBFAIL, C_ADEC, C_ADECFAIL, C_IDRRETRY, C_REBUILD, C_AFRAMES, C_N };
static unsigned s_c[C_N];
static unsigned long long s_abytes;       /* PCM bytes handed to the audio callback */
static int s_adrops, s_asent, s_asendfail;   /* the host's audio counters (last report): dropped, sent to the UI, failed sends */
static int s_aout_off = -1, s_aout_end;   /* where the aout section sits in s_out (-1 = none); mldiag_last re-renders it */
static int s_adec_err;                    /* last libopus error */
static int s_aout;                        /* audio output: 0 not reached, 1 callback present, 2 none (no callback), 3 native, 4 native -> callback fallback */
static char s_aio_reason[48] = "-";       /* why the native output is not used (audio_opus.c token) */
static unsigned s_an_frames, s_an_under, s_an_over, s_an_drops;   /* native audio-io counters (last report kept) */
static int s_an_bufms;                    /* ring-buffer fill in ms at the last report */
static int s_pad_rc;                      /* last failed pad call rc */
static int s_ctl_iv = -1;                 /* last control-stream IV length, -1 = none */
/* the noted /launch or /resume (survives mldiag_reset until one reset consumes it) */
static int s_ln_mode;                     /* 0 none, 1 launch, 2 resume */
static int s_ln_keyok, s_ln_idok, s_ln_id;
static unsigned char s_ln_key[16];
static char s_launch[8] = "-", s_rikey[8] = "-", s_rikeyid[8] = "-";   /* per session result */
#define INC(i) __atomic_fetch_add(&s_c[i], 1, __ATOMIC_RELAXED)
#define GET_A(v) __atomic_load_n(&(v), __ATOMIC_RELAXED)
#define GET(i) __atomic_load_n(&s_c[i], __ATOMIC_RELAXED)

static long long now_ms(void) {
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void mldiag_note_launch(const char *url) {
  if (!url) return;
  pthread_mutex_lock(&s_mu);
  s_ln_mode = strstr(url, "/resume?") ? 2 : 1;
  s_ln_keyok = s_ln_idok = 0;
  const char *k = strstr(url, "rikey=");
  if (k) {
    k += 6;
    int n = 0;
    while (n < 32 && ((k[n] >= '0' && k[n] <= '9') || (k[n] >= 'a' && k[n] <= 'f') || (k[n] >= 'A' && k[n] <= 'F'))) n++;
    if (n == 32 && (k[32] == 0 || k[32] == '&')) {
      for (int i = 0; i < 16; i++) { char h[3] = { k[2 * i], k[2 * i + 1], 0 }; s_ln_key[i] = (unsigned char)strtoul(h, NULL, 16); }
      s_ln_keyok = 1;
    }
  }
  const char *id = strstr(url, "rikeyid=");
  if (id) { s_ln_id = (int)strtol(id + 8, NULL, 10); s_ln_idok = 1; }
  pthread_mutex_unlock(&s_mu);
}

unsigned mldiag_ctl_encrypt(int ok, int ivlen, int verify) {
  unsigned n = INC(C_CTLENC) + 1;
  if (!ok) INC(C_CTLFAIL);
  __atomic_store_n(&s_ctl_iv, ivlen, __ATOMIC_RELAXED);
  INC(verify > 0 ? C_VOK : verify == 0 ? C_VFAIL : C_VNA);
  return n;
}

void mldiag_pad(int arrival, int rc) {
  INC(arrival ? C_PADARR : C_PADST);
  if (rc != 0) { INC(C_PADFAIL); __atomic_store_n(&s_pad_rc, rc, __ATOMIC_RELAXED); }
}

void mldiag_aout(int has_cb) { __atomic_store_n(&s_aout, has_cb ? 1 : 2, __ATOMIC_RELAXED); }
void mldiag_aout_native(int active) { __atomic_store_n(&s_aout, active ? 3 : 4, __ATOMIC_RELAXED); }
/* Why the native output is not used; shown in the "aout webaudio fallback=native" variant.
   The audio_opus.c token (no-library, no-symbols, channels-N, create-N, prepare-N, write-N, oom, no-thread). */
void mldiag_aout_reason(const char *reason) {
  pthread_mutex_lock(&s_mu);
  snprintf(s_aio_reason, sizeof s_aio_reason, "%s", reason && *reason ? reason : "-");
  for (char *p = s_aio_reason; *p; p++) if (*p == '\n' || *p == '\r' || *p == '|') *p = ' ';   /* one line, no section breaks */
  pthread_mutex_unlock(&s_mu);
}
/* Native counters: written by the audio worker once per event (cheap stores). */
void mldiag_anative(unsigned frames, unsigned underruns, unsigned overruns, unsigned drops, int buf_ms) {
  __atomic_store_n(&s_an_frames, frames, __ATOMIC_RELAXED);
  __atomic_store_n(&s_an_under, underruns, __ATOMIC_RELAXED);
  __atomic_store_n(&s_an_over, overruns, __ATOMIC_RELAXED);
  __atomic_store_n(&s_an_drops, drops, __ATOMIC_RELAXED);
  __atomic_store_n(&s_an_bufms, buf_ms, __ATOMIC_RELAXED);
}
void mldiag_aframe(int bytes) { INC(C_AFRAMES); __atomic_fetch_add(&s_abytes, (unsigned long long)bytes, __ATOMIC_RELAXED); }
void mldiag_adrops(int dropped) { __atomic_store_n(&s_adrops, dropped, __ATOMIC_RELAXED); }
void mldiag_ahost(int sent, int dropped, int send_failures) {
  __atomic_store_n(&s_asent, sent, __ATOMIC_RELAXED); __atomic_store_n(&s_adrops, dropped, __ATOMIC_RELAXED);
  __atomic_store_n(&s_asendfail, send_failures, __ATOMIC_RELAXED);
}
void mldiag_idr_retry(void) { INC(C_IDRRETRY); }
void mldiag_rebuild(void) { INC(C_REBUILD); }
void mldiag_catchup(int depth_ms, int seek_ok) {
  pthread_mutex_lock(&s_mu); s_cu_n++; s_cu_depth = depth_ms; s_cu_rc = seek_ok; pthread_mutex_unlock(&s_mu);
}

void mldiag_osd(int want, int win) {
  pthread_mutex_lock(&s_mu); s_osd_want = want; s_osd_win = win; pthread_mutex_unlock(&s_mu);
}

void mldiag_adec(int ok, int err) {
  if (ok) { INC(C_ADEC); return; }
  INC(C_ADECFAIL); __atomic_store_n(&s_adec_err, err, __ATOMIC_RELAXED);
}

void mldiag_reset(const mldiag_cfg *cfg) {
  pthread_mutex_lock(&s_mu);
  memset(s_c, 0, sizeof s_c); s_ctl_iv = -1; s_pad_rc = 0; s_adec_err = 0;
  s_aout = 0; s_abytes = 0; s_adrops = s_asent = s_asendfail = 0; s_aout_off = -1; s_aio_reason[0] = '-'; s_aio_reason[1] = 0;
  s_an_frames = s_an_under = s_an_over = s_an_drops = 0; s_an_bufms = 0;
  s_rb_open = s_rb_addv = s_rb_disp = s_rb_prep = -99;
  s_cu_n = 0; s_cu_depth = -1; s_cu_rc = -99; s_osd_win = 0;
  memset(s_first, 0, sizeof s_first); memset(s_last, 0, sizeof s_last);
  s_nlines = 0; s_out[0] = 0; s_code = 0;
  s_vset = 0; s_open = s_win = s_prep = s_pdone = s_start = s_state = -99; s_first_pkt_ms = -1; s_sub_last_bad = s_vfail_last = s_afail_last = 0;
  s_t_prep = s_t_pdone = s_t_err = s_t_v1 = s_t_a1 = s_vpts1 = s_apts1 = -1; s_first_du_ms = -1;
  memset(&s_cfg, 0, sizeof s_cfg);
  if (cfg) {
    s_cfg = *cfg;
    snprintf(s_appver, sizeof s_appver, "%s", cfg->app_version ? cfg->app_version : "");
    snprintf(s_gfever, sizeof s_gfever, "%s", cfg->gfe_version ? cfg->gfe_version : "");
  } else s_appver[0] = s_gfever[0] = 0;
  snprintf(s_host, sizeof s_host, "%s", cfg && cfg->host ? cfg->host : "");
  s_cfg.host = NULL;   /* never kept or printed */
  s_cfg.app_version = s_appver; s_cfg.gfe_version = s_gfever;
  snprintf(s_rtsp, sizeof s_rtsp, "%s", cfg && cfg->rtsp_scheme ? cfg->rtsp_scheme : "-");
  snprintf(s_launch, sizeof s_launch, "-"); snprintf(s_rikey, sizeof s_rikey, "-"); snprintf(s_rikeyid, sizeof s_rikeyid, "-");
  if (cfg && s_ln_mode) {
    snprintf(s_launch, sizeof s_launch, "%s", s_ln_mode == 2 ? "resume" : "launch");
    if (!s_ln_keyok) snprintf(s_rikey, sizeof s_rikey, "bad");
    else if (cfg->aes_key) snprintf(s_rikey, sizeof s_rikey, "%s", memcmp(cfg->aes_key, s_ln_key, 16) ? "DIFF" : "ok");
    if (s_ln_idok) snprintf(s_rikeyid, sizeof s_rikeyid, "%s", cfg->rikeyid == s_ln_id ? "ok" : "DIFF");
  }
  if (cfg) { s_ln_mode = 0; memset(s_ln_key, 0, sizeof s_ln_key); }   /* one launch per session */
  s_t0 = now_ms();
  pthread_mutex_unlock(&s_mu);
}

void mldiag_cc_line(const char *line) {
  if (!line) return;
  char b[LINE_MAX_CC + 1], m[2048];
  pthread_mutex_lock(&s_mu);
  size_t hl = strlen(s_host), mn = 0;
  if (hl) {   /* mask the whole line before the cut, so a cut-off address cannot slip through */
    for (const char *p = line; *p && mn < sizeof m - 6; ) {
      if (!strncmp(p, s_host, hl)) { memcpy(m + mn, "<host>", 6); mn += 6; p += hl; }
      else m[mn++] = *p++;
    }
    m[mn] = 0; line = m;
  }
  pthread_mutex_unlock(&s_mu);
  size_t n = strlen(line);
  while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
  if (n > LINE_MAX_CC) n = LINE_MAX_CC;
  memcpy(b, line, n); b[n] = 0;
  static const char FP[] = "Received first video packet after ";
  const char *fp = strstr(b, FP);
  int ms;
  if (fp && sscanf(fp + sizeof FP - 1, "%d", &ms) == 1) __atomic_store_n(&s_first_pkt_ms, ms, __ATOMIC_RELAXED);
  else if (strstr(b, "Unrecoverable frame")) INC(C_UNREC);
  else if (strstr(b, "Network dropped")) INC(C_DROP);
  else if (strstr(b, "Waiting for IDR frame")) INC(C_WAITIDR);
  else if (strstr(b, "Invalid last payload length")) INC(C_BADLEN);
  else if (strstr(b, "Failed to decrypt") || strstr(b, "Discarding unencrypted packet")) INC(C_DECRYPT);   /* video and control */
  else if (strstr(b, "Depacketizer detected corrupt frame") || strstr(b, "FEC recovery returned corrupt") ||
           strstr(b, "Recovery error")) INC(C_CORRUPT);
  else if (strstr(b, "Unrecognized frame type")) INC(C_UNKFT);
  else if (strstr(b, "Requesting IDR frame") || strstr(b, "IDR frame request sent") || strstr(b, "Sending RFI request") ||
           strstr(b, "Sending speculative RFI")) INC(C_IDRREQ);
  else if (strstr(b, "Recovered ")) INC(C_FEC);
  else if (strstr(b, "Video decode unit queue overflow")) INC(C_QOVF);
  else if (strstr(b, "Reached consecutive drop limit")) INC(C_DROPLIM);
  pthread_mutex_lock(&s_mu);
  if (s_nlines < FIRST_N) memcpy(s_first[s_nlines], b, n + 1);
  memcpy(s_last[s_nlines % LAST_N], b, n + 1);
  s_nlines++;
  pthread_mutex_unlock(&s_mu);
}

void mldiag_video_setup(int fmt, int w, int h, int fps, int rc) {
  pthread_mutex_lock(&s_mu);
  s_vfmt = fmt; s_vw = w; s_vh = h; s_vfps = fps; s_vrc = rc; s_vset = 1;
  pthread_mutex_unlock(&s_mu);
}

void mldiag_video_du(int is_idr) {
  INC(C_DU);
  if (is_idr) INC(C_IDR);
  pthread_mutex_lock(&s_mu);   /* once per frame: cheap */
  if (s_first_du_ms < 0) s_first_du_ms = now_ms() - s_t0;
  pthread_mutex_unlock(&s_mu);
}

void mldiag_submit(int is_video, int ok, int status, unsigned long long pts_ms) {
  INC(is_video ? C_VSUB : C_ASUB);
  if (!ok) {
    INC(C_SUBFAIL); __atomic_store_n(&s_sub_last_bad, status, __ATOMIC_RELAXED);
    INC(is_video ? C_VSUBFAIL : C_ASUBFAIL); __atomic_store_n(is_video ? &s_vfail_last : &s_afail_last, status, __ATOMIC_RELAXED);
  } else if (__atomic_load_n(is_video ? &s_t_v1 : &s_t_a1, __ATOMIC_RELAXED) < 0) {   /* first success of this stream */
    pthread_mutex_lock(&s_mu);
    long long *t = is_video ? &s_t_v1 : &s_t_a1;
    if (*t < 0) { *(is_video ? &s_vpts1 : &s_apts1) = (long long)pts_ms; __atomic_store_n(t, now_ms(), __ATOMIC_RELAXED); }
    pthread_mutex_unlock(&s_mu);
  }
}

void mldiag_player(const char *what, int rc) {
  pthread_mutex_lock(&s_mu);
  if (!strcmp(what, "open")) s_open = rc;
  else if (!strcmp(what, "pdone")) { s_pdone = rc; if (s_t_pdone < 0) s_t_pdone = now_ms(); }
  else if (!strcmp(what, "err")) { if (s_t_err < 0) s_t_err = now_ms(); }   /* first player error only; the value is in err=<n>/<last> */
  else if (!strcmp(what, "win")) s_win = rc; else if (!strcmp(what, "prep")) { s_prep = rc; s_t_prep = now_ms(); } else if (!strcmp(what, "start")) s_start = rc;
  else if (!strcmp(what, "state")) s_state = rc;
  else if (!strcmp(what, "rb_open")) s_rb_open = rc; else if (!strcmp(what, "rb_addv")) s_rb_addv = rc;
  else if (!strcmp(what, "rb_disp")) s_rb_disp = rc; else if (!strcmp(what, "rb_prep")) s_rb_prep = rc;
  pthread_mutex_unlock(&s_mu);
}

/* The aout section. The host reports its final audio counters after the failure and before it reads the line, so
   mldiag_last renders this section again. */
static void aout_text(char *d, size_t n) {
  int a = GET_A(s_aout);
  if (a == 3)
    snprintf(d, n, " | aout native frames=%u underruns=%u overruns=%u drops=%u buf_ms=%d",
             GET_A(s_an_frames), GET_A(s_an_under), GET_A(s_an_over), GET_A(s_an_drops), GET_A(s_an_bufms));
  else if (a == 4)   /* native output failed: this session fell back to the callback path */
    snprintf(d, n, " | aout webaudio fallback=native reason=%s frames=%u bytes=%llu sent=%d drops=%d sendfail=%d",
             s_aio_reason, GET(C_AFRAMES), GET_A(s_abytes), GET_A(s_asent), GET_A(s_adrops), GET_A(s_asendfail));
  else if (a == 1)
    snprintf(d, n, " | aout webaudio frames=%u bytes=%llu sent=%d drops=%d sendfail=%d", GET(C_AFRAMES), GET_A(s_abytes), GET_A(s_asent), GET_A(s_adrops), GET_A(s_asendfail));
  else
    snprintf(d, n, " | aout none reason=%s", a == 2 ? "no-callback" : "-");
}

static const char *code_name(int c) {
  switch (c) {
    case 0: return "GRACEFUL"; case -100: return "NO_VIDEO_TRAFFIC"; case -101: return "NO_VIDEO_FRAME";
    case -102: return "UNEXPECTED_EARLY_TERMINATION"; case -103: return "PROTECTED_CONTENT";
    case -104: return "FRAME_CONVERSION";
    case -110: return "PLAYER_NOT_STARTED"; case -111: return "PLAYER_STALLED"; default: return "OTHER";
  }
}

/* snprintf that advances and never overruns */
#define ADD(...) do { if (o < OUT_MAX - 1) { int r_ = snprintf(s_out + o, OUT_MAX - o, __VA_ARGS__); \
                      if (r_ > 0) { o += r_; if (o > OUT_MAX - 1) o = OUT_MAX - 1; } } } while (0)

static void add_lines(int *po, char (*ring)[LINE_MAX_CC + 1], int ringn, int start, int count) {
  int o = *po;
  for (int i = 0; i < count; i++) ADD("%s%s", i ? " ; " : "", ring[(start + i) % ringn]);
  *po = o;
}

void mldiag_fail(int code, int stage, const char *stage_name, int host_feature_flags,
                 int rtt, int rtt_var, unsigned player_errors, unsigned player_last_error,
                 unsigned enc_supported, unsigned enc_requested, unsigned enc_enabled) {
  long long t = now_ms();
  pthread_mutex_lock(&s_mu);
  int o = 0;
  s_out[0] = 0;
  ADD("ML-ERROR code=%d name=%s stage=%s", code, code_name(code), stage >= 0 && stage_name ? stage_name : "-");
  ADD(" t=%lldms | cfg %dx%d@%d br=%d pkt=%d fmts=0x%x cs=%d cr=%d audio=0x%x scm=0x%x enc=0x%x encf=0x%x/0x%x/0x%x rtsp=%s launch=%s rikey=%s rikeyid=%s app=%s gfe=%s hostflags=0x%x rtt=%d/%d",
      t - s_t0, s_cfg.width, s_cfg.height, s_cfg.fps, s_cfg.bitrate_kbps, s_cfg.packet_size,
      s_cfg.supported_video_formats, s_cfg.color_space, s_cfg.color_range, s_cfg.audio_config,
      s_cfg.server_codec_mode_support, s_cfg.encryption_flags, enc_supported, enc_requested, enc_enabled, s_rtsp, s_launch, s_rikey, s_rikeyid, s_appver, s_gfever,
      host_feature_flags, rtt, rtt_var);
  long long fdu = s_first_du_ms;
  char fduS[24], fpS[24];
  if (fdu < 0) snprintf(fduS, sizeof fduS, "-"); else snprintf(fduS, sizeof fduS, "%lld", fdu);
  if (s_first_pkt_ms < 0) snprintf(fpS, sizeof fpS, "-"); else snprintf(fpS, sizeof fpS, "%d", s_first_pkt_ms);
  if (s_vset) ADD(" | video setup=0x%x %dx%d@%d rc=%d", s_vfmt, s_vw, s_vh, s_vfps, s_vrc);
  else ADD(" | video setup=none");
  ADD(" du=%u idr=%u first_du_ms=%s first_pkt_ms=%s idr_retry=%u", GET(C_DU), GET(C_IDR), fduS, fpS, GET(C_IDRRETRY));
  ADD(" | player open=%d win=%d prep=%d pdone=%d start=%d state=%d rebuilds=%u rb=%d/%d/%d/%d vsub=%u asub=%u subfail=%u/%d vfail=%u/%d afail=%u/%d adec=%u adec_fail=%u/%d err=%u/%u", s_open, s_win, s_prep, s_pdone, s_start, s_state, GET(C_REBUILD), s_rb_open, s_rb_addv, s_rb_disp, s_rb_prep,
      GET(C_VSUB), GET(C_ASUB), GET(C_SUBFAIL), s_sub_last_bad, GET(C_VSUBFAIL), s_vfail_last, GET(C_ASUBFAIL), s_afail_last,
      GET(C_ADEC), GET(C_ADECFAIL), __atomic_load_n(&s_adec_err, __ATOMIC_RELAXED), player_errors, player_last_error);
  if (s_cu_n) ADD(" catchup=%d/%d/%d", s_cu_n, s_cu_depth, s_cu_rc); else ADD(" catchup=0/-/-");
  ADD(" osd=%d/%s", s_osd_want, s_osd_win == 1 ? "lowered" : s_osd_win == 2 ? "raised" : s_osd_win < 0 ? "nosym" : "-");
  if (ml_engine_get_target_depth_ms) ADD(" target=%d", ml_engine_get_target_depth_ms()); else ADD(" target=-");
  char tS[4][24], pS[2][24];   /* ms after prepare_async, "-" = not reached */
  const long long ev[4] = { s_t_pdone, s_t_err, s_t_v1, s_t_a1 }, pv[2] = { s_vpts1, s_apts1 };
  for (int i = 0; i < 4; i++) { if (ev[i] < 0 || s_t_prep < 0) snprintf(tS[i], sizeof tS[i], "-"); else snprintf(tS[i], sizeof tS[i], "%lld", ev[i] - s_t_prep); }
  for (int i = 0; i < 2; i++) { if (pv[i] < 0) snprintf(pS[i], sizeof pS[i], "-"); else snprintf(pS[i], sizeof pS[i], "%lld", pv[i]); }
  ADD(" | prep-t pdone=%s err=%s v1=%s a1=%s vpts1=%s apts1=%s", tS[0], tS[1], tS[2], tS[3], pS[0], pS[1]);
  char aS[160];
  aout_text(aS, sizeof aS);
  s_aout_off = o;
  ADD("%s", aS);
  s_aout_end = o;
  char ivS[16];
  int civ = __atomic_load_n(&s_ctl_iv, __ATOMIC_RELAXED);
  if (civ < 0) snprintf(ivS, sizeof ivS, "-"); else snprintf(ivS, sizeof ivS, "%d", civ);
  ADD(" | ctl enc=%u fail=%u iv=%s verify=%u/%u/%u", GET(C_CTLENC), GET(C_CTLFAIL), ivS, GET(C_VOK), GET(C_VFAIL), GET(C_VNA));
  ADD(" | pad arr=%u st=%u fail=%u/%d", GET(C_PADARR), GET(C_PADST), GET(C_PADFAIL), __atomic_load_n(&s_pad_rc, __ATOMIC_RELAXED));
  ADD(" | cc unrec=%u drop=%u waitidr=%u badlen=%u decrypt=%u corrupt=%u unkft=%u idrreq=%u fec=%u qovf=%u droplim=%u",
      GET(C_UNREC), GET(C_DROP), GET(C_WAITIDR), GET(C_BADLEN), GET(C_DECRYPT), GET(C_CORRUPT), GET(C_UNKFT),
      GET(C_IDRREQ), GET(C_FEC), GET(C_QOVF), GET(C_DROPLIM));
  int nf = s_nlines < FIRST_N ? s_nlines : FIRST_N;
  ADD(" | cc-first: ");
  add_lines(&o, s_first, FIRST_N, 0, nf);
  int nl = s_nlines < LAST_N ? s_nlines : LAST_N;
  ADD(" | cc-last: ");
  add_lines(&o, s_last, LAST_N, s_nlines - nl, nl);
  for (char *p = s_out; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
  s_code = code ? code : -1;   /* a failure always reports non-zero */
  s_out[OUT_MAX - 1] = 0;
  fprintf(stderr, "[ml] %s\n", s_out);
  pthread_mutex_unlock(&s_mu);
}

int mldiag_last(char *buf, int len) {
  pthread_mutex_lock(&s_mu);
  int c = s_code;
  if (buf && len > 0) {
    if (s_aout_off >= 0 && s_aout_end > s_aout_off) {
      char aS[160];
      aout_text(aS, sizeof aS);
      snprintf(buf, (size_t)len, "%.*s%s%s", s_aout_off, s_out, aS, s_out + s_aout_end);
    } else snprintf(buf, (size_t)len, "%s", s_out);
  }
  pthread_mutex_unlock(&s_mu);
  return c;
}

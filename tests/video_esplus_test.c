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
#include "h264_sps.h"
#include "mldiag.h"
#include "hevc_sei.h"
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
static void (*async_cb)(void *); static void *async_data;   /* queued; run_async() plays the main loop */
void ecore_main_loop_thread_safe_call_async(void (*cb)(void *), void *data) { async_cb = cb; async_data = data; }
static void run_async(void) { void (*c)(void *) = async_cb; async_cb = NULL; if (c) c(async_data); }
static int pl, create_calls, stop_calls, addv_ok = 1;
esplusplayer_handle esplusplayer_create(void) { create_calls++; return (esplusplayer_handle)&pl; }
int esplusplayer_open(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_close(esplusplayer_handle h) { (void)h; return 1; }
int esplusplayer_stop(esplusplayer_handle h) { (void)h; stop_calls++; return 1; }
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
static int start_ret = 1;
int esplusplayer_start(esplusplayer_handle h) { (void)h; return start_ret; }
int esplusplayer_get_state(esplusplayer_handle h) { (void)h; return 2; }
int esplusplayer_set_ecore_display(esplusplayer_handle h, esplusplayer_display_type t, void *win, int x, int y, int ww, int hh) { (void)h;(void)t;(void)win;(void)x;(void)y;(void)ww;(void)hh; return 1; }
int esplusplayer_set_display_mode(esplusplayer_handle h, esplusplayer_display_mode m) { (void)h;(void)m; return 1; }
static int seek_calls; static uint64_t seek_target;
int esplusplayer_seek(esplusplayer_handle h, uint64_t t) { (void)h; seek_calls++; seek_target = t; return 1; }
void esplusplayer_set_seek_done_cb(esplusplayer_handle h, esplusplayer_seek_done_cb c) { (void)h;(void)c; }
int esplusplayer_add_video_stream_info(esplusplayer_handle h, const esplusplayer_video_stream_info *i) { (void)h;(void)i; return addv_ok; }
static uint64_t play_ms = 1;
int esplusplayer_get_playing_time(esplusplayer_handle h, uint64_t *t) { (void)h; *t = play_ms; return 1; }
/* submit stub: record packets; return script[0..n_script) in order, then SUCCESS */
#define MAXP 64
static int sub_calls, script[16], n_script; static uint64_t rec_ms[MAXP];
static uint8_t last_pkt[1024]; static size_t last_len;   /* bytes of the last submitted packet */
int esplusplayer_submit_packet(esplusplayer_handle h, const esplusplayer_es_packet *p) {
  (void)h; int r = sub_calls < n_script ? script[sub_calls] : ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS;
  assert(p->type == ESPLUSPLAYER_STREAM_TYPE_VIDEO && sub_calls < MAXP);
  assert(p->buffer_size <= sizeof last_pkt); memcpy(last_pkt, p->buffer, p->buffer_size); last_len = p->buffer_size;
  rec_ms[sub_calls++] = p->pts / 1000000; return r;
}
static int idr_calls;
void LiRequestIdrFrame(void) { idr_calls++; }
void LiInitializeVideoCallbacks(PDECODER_RENDERER_CALLBACKS c) { memset(c, 0, sizeof *c); }
extern void VidEsplus_FillCallbacks(PDECODER_RENDERER_CALLBACKS cb);

static DECODER_RENDERER_CALLBACKS cb;
static int vfmt = VIDEO_FORMAT_H264;
static void script_set(int n, int st) { n_script = n; for (int i = 0; i < n; i++) script[i] = st; }
/* fresh session: mode NULL = explicit "off", "unset" = env not set (the default); depth thread off; player prepared */
static void begin(const char *mode, const char *drop) {
  sub_calls = 0; n_script = 0; idr_calls = 0; play_ms = 1;
  setenv("MLENGINE_CATCHUP_MS", "0", 1);   /* depth 900 in the nudge test: no catch-up seek */
  setenv("MLENGINE_DEPTH_POLL_MS", "0", 1);
  if (mode && !strcmp(mode, "unset")) unsetenv("MLENGINE_LATENCY_MODE"); else setenv("MLENGINE_LATENCY_MODE", mode ? mode : "off", 1);
  if (drop) setenv("MLENGINE_DROP_AFTER_MS", drop, 1); else unsetenv("MLENGINE_DROP_AFTER_MS");
  VidEsplus_FillCallbacks(&cb);
  assert(cb.setup(vfmt, 1280, 720, 60, NULL, 0) == 0);
  cb.start(); cb_prepared(1);
}
static void end(void) { cb.cleanup(); }
static int frame(unsigned host_ms, int type) {
  static char data[16]; LENTRY e = {NULL, data, sizeof data, 0};
  DECODE_UNIT du; memset(&du, 0, sizeof du);
  du.presentationTimeMs = host_ms; du.frameType = type; du.fullLength = sizeof data; du.bufferList = &e;
  return cb.submitDecodeUnit(&du);
}
/* an IDR decode unit of three entries: SPS, PPS, picture data */
static const uint8_t SPS_NR[] = { 0, 0, 0, 1, 0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16, 0xE4 };   /* baseline, no VUI */
static const uint8_t PPS_B[] = { 0, 0, 0, 1, 0x68, 0xCE, 0x3C, 0x80 };
static const uint8_t PIC_B[] = { 0, 0, 0, 1, 0x65, 0x88, 0x84, 0x00, 0x33, 0xFF };
static int idr3(const uint8_t *sps, size_t sn) {
  LENTRY pic = {NULL, (char *)PIC_B, sizeof PIC_B, BUFFER_TYPE_PICDATA}, pps = {&pic, (char *)PPS_B, sizeof PPS_B, BUFFER_TYPE_PPS},
         e = {&pps, (char *)sps, (int)sn, BUFFER_TYPE_SPS};
  DECODE_UNIT du; memset(&du, 0, sizeof du);
  du.frameType = FRAME_TYPE_IDR; du.fullLength = (int)(sn + sizeof PPS_B + sizeof PIC_B); du.bufferList = &e;
  return cb.submitDecodeUnit(&du);
}
/* last_pkt must be: sps_out, then the PPS and the picture data byte for byte */
static void check_pkt(const uint8_t *sps_out, size_t sn) {
  assert(last_len == sn + sizeof PPS_B + sizeof PIC_B);
  assert(!memcmp(last_pkt, sps_out, sn));
  assert(!memcmp(last_pkt + sn, PPS_B, sizeof PPS_B) && !memcmp(last_pkt + sn + sizeof PPS_B, PIC_B, sizeof PIC_B));
}
#define P FRAME_TYPE_PFRAME
/* the HEVC SEI scanner: VPS/SPS/PPS + prefix SEI (137: 24 bytes, 144: 4 bytes) + IDR slice */
static void sei_test(void) {
  uint8_t b[128]; size_t n = 0; int mask;
  const uint8_t ps[] = {0,0,0,1,0x40,1,0xaa, 0,0,0,1,0x42,1,0xbb, 0,0,0,1,0x44,1,0xcc};
  memcpy(b, ps, sizeof ps); n = sizeof ps;
  size_t sei0 = n;
  const uint8_t h[] = {0,0,0,1,0x4e,1, 137,24}; memcpy(b + n, h, sizeof h); n += sizeof h; memset(b + n, 7, 24); n += 24;
  const uint8_t c[] = {144,4}; memcpy(b + n, c, sizeof c); n += sizeof c; memset(b + n, 9, 4); n += 4;
  b[n++] = 0x80;
  size_t sei_end = n;
  const uint8_t idr[] = {0,0,1,0x26,1,0xdd,0xee}; memcpy(b + n, idr, sizeof idr); n += sizeof idr;
  mask = 0; assert(hevc_sei_scan(b, n, &mask) == 1 && mask == 3);
  mask = 0; assert(hevc_sei_scan(b, sei0, &mask) == 0 && mask == 0);   /* no SEI */
  /* truncated at every length (heap copy: ASan sees an over-read) */
  for (size_t k = 0; k <= n; k++) { uint8_t *t = malloc(k ? k : 1); memcpy(t, b, k); mask = 0; assert(hevc_sei_scan(t, k, &mask) <= 1); free(t); }
  { uint8_t *t = malloc(sei_end - 3); memcpy(t, b, sei_end - 3); mask = 0;   /* SEI cut inside the 144 payload */
    assert(hevc_sei_scan(t, sei_end - 3, &mask) == 1 && mask == 3); free(t); }
  { const uint8_t bad[] = {0,0,1,0x4e,1,0xff,0xff,0xff}; mask = 0; assert(hevc_sei_scan(bad, sizeof bad, &mask) == 1 && mask == 0); }
}

extern int ml_engine_set_hdr_test(int force, int sei, int color);
extern int ml_engine_get_hdr_test(void);
static int color_calls; static char color_txt[512];   /* the linker wraps esplus_set_color_info (build script) */
void __wrap_esplus_set_color_info(const char *t) { color_calls++; snprintf(color_txt, sizeof color_txt, "%s", t); }
static int hdr_on = 1, meta_ok = 1;
bool LiGetCurrentHostDisplayHdrMode(void) { return hdr_on; }
bool LiGetHdrMetadata(PSS_HDR_METADATA m) {
  if (!meta_ok) return false;
  memset(m, 0, sizeof *m);
  m->displayPrimaries[0].x = 34000; m->displayPrimaries[0].y = 16000;   /* R */
  m->displayPrimaries[1].x = 13250; m->displayPrimaries[1].y = 34500;   /* G */
  m->displayPrimaries[2].x = 7500;  m->displayPrimaries[2].y = 3000;    /* B */
  m->whitePoint.x = 15635; m->whitePoint.y = 16450;
  m->maxDisplayLuminance = 1000; m->minDisplayLuminance = 50; m->maxContentLightLevel = 800; m->maxFrameAverageLightLevel = 400;
  return true;
}
/* undo emulation prevention of a built NAL (after the start code) */
static size_t unescape(const uint8_t *s, size_t n, uint8_t *o) {
  size_t k = 0; int z = 0;
  for (size_t i = 0; i < n; i++) { if (z >= 2 && s[i] == 3) { z = 0; continue; } o[k++] = s[i]; z = s[i] ? 0 : z + 1; }
  return k;
}
static void sei_build_test(void) {
  uint8_t o[HEVC_SEI_MAX], r[HEVC_SEI_MAX]; int mask = 0;
  const uint16_t rgb[6] = {34000, 16000, 13250, 34500, 7500, 3000};
  size_t n = hevc_sei_build(o, sizeof o, rgb, 15635, 16450, 1000, 50, 800, 400);
  assert(n > 0 && n <= HEVC_SEI_MAX && !memcmp(o, "\0\0\0\1\x4e\x01", 6));
  assert(hevc_sei_scan(o, n, &mask) == 1 && (mask & 1));
  size_t k = unescape(o + 4, n - 4, r);
  assert(k == 35 && r[2] == 137 && r[3] == 24 && r[28] == 144 && r[29] == 4 && r[34] == 0x80);
  const uint8_t *p = r + 4;
#define U16(q) (unsigned)((q)[0] << 8 | (q)[1])
  assert(U16(p) == 13250 && U16(p + 2) == 34500 && U16(p + 4) == 7500 && U16(p + 6) == 3000 && U16(p + 8) == 34000 && U16(p + 10) == 16000);   /* G, B, R */
  assert(U16(p + 12) == 15635 && U16(p + 14) == 16450);
  assert(U16(p + 16) == 0x0098 && U16(p + 18) == 0x9680);   /* 1000 nits = 10,000,000 = 0x00989680 */
  assert(U16(p + 20) == 0 && U16(p + 22) == 50 && U16(r + 30) == 800 && U16(r + 32) == 400);
  { const uint16_t z[6] = {0};   /* zeros: emulation prevention must kick in */
    n = hevc_sei_build(o, sizeof o, z, 0, 0, 1, 0, 0, 0);
    assert(n > 39 && n <= HEVC_SEI_MAX);
    for (size_t i = 4; i + 2 < n; i++) assert(!(o[i] == 0 && o[i + 1] == 0 && o[i + 2] <= 2));
    assert(unescape(o + 4, n - 4, r) == 35);
    mask = 0; assert(hevc_sei_scan(o, n, &mask) == 1 && (mask & 1)); }
  n = hevc_sei_build(o, sizeof o, rgb, 1, 1, 1, 1, 1, 1);
  for (size_t c = 0; c < n; c++) assert(hevc_sei_build(o, c, rgb, 1, 1, 1, 1, 1, 1) == 0);   /* cap too small */
#undef U16
}
/* HEVC IDR: SPS, PPS, picture data; the SEI must land right before the picture data */
static int idr_hevc(const uint8_t *pic, size_t pn) {
  LENTRY pc = {NULL, (char *)pic, (int)pn, BUFFER_TYPE_PICDATA}, pps = {&pc, (char *)PPS_B, sizeof PPS_B, BUFFER_TYPE_PPS},
         e = {&pps, (char *)SPS_NR, sizeof SPS_NR, BUFFER_TYPE_SPS};
  DECODE_UNIT du; memset(&du, 0, sizeof du);
  du.frameType = FRAME_TYPE_IDR; du.fullLength = (int)(sizeof SPS_NR + sizeof PPS_B + pn); du.bufferList = &e;
  return cb.submitDecodeUnit(&du);
}
static unsigned ins_count(void) {
  char line[2048]; mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
  const char *s = strstr(line, " ins="); assert(s); return (unsigned)atoi(s + 5);
}
static void sei_insert_test(void) {
  static const uint8_t SLICE[] = {0, 0, 0, 1, 0x26, 0x01, 0xAF, 0x08};
  uint8_t want[HEVC_SEI_MAX]; const uint16_t rgb[6] = {34000, 16000, 13250, 34500, 7500, 3000};
  size_t sn = hevc_sei_build(want, sizeof want, rgb, 15635, 16450, 1000, 50, 800, 400), pre = sizeof SPS_NR + sizeof PPS_B;
  vfmt = VIDEO_FORMAT_H265;
  begin(NULL, NULL); mldiag_reset(NULL);   /* on: SEI right before the picture data, once per frame */
  assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && last_len == pre + sn + sizeof SLICE);
  assert(!memcmp(last_pkt + pre, want, sn) && !memcmp(last_pkt + pre + sn, SLICE, sizeof SLICE) && !memcmp(last_pkt + pre - sizeof PPS_B, PPS_B, sizeof PPS_B));
  assert(ins_count() == 1);
  assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && ins_count() == 2);
  end();
  hdr_on = 0; begin(NULL, NULL); mldiag_reset(NULL);   /* host not in HDR mode */
  assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && last_len == pre + sizeof SLICE && ins_count() == 0);
  end(); hdr_on = 1;
  meta_ok = 0; begin(NULL, NULL); mldiag_reset(NULL);   /* no metadata */
  assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && last_len == pre + sizeof SLICE && ins_count() == 0);
  end(); meta_ok = 1;
  { uint8_t has[sizeof SLICE + 40]; size_t n = 0;   /* the host already sent 137 */
    const uint8_t s[] = {0, 0, 0, 1, 0x4e, 1, 137, 24}; memcpy(has, s, sizeof s); n = sizeof s; memset(has + n, 7, 24); n += 24; has[n++] = 0x80;
    memcpy(has + n, SLICE, sizeof SLICE); n += sizeof SLICE;
    begin(NULL, NULL); mldiag_reset(NULL);
    assert(idr_hevc(has, n) == DR_OK && last_len == pre + n && !memcmp(last_pkt + pre, has, n) && ins_count() == 0);
    end(); }
  setenv("MLENGINE_HDR_SEI", "0", 1); begin(NULL, NULL); mldiag_reset(NULL);   /* switch off */
  assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && last_len == pre + sizeof SLICE && ins_count() == 0);
  end(); unsetenv("MLENGINE_HDR_SEI");
  /* HDR test mode (ml_engine_set_hdr_test): force = fixed HDR10 metadata when the host is not in HDR mode */
  { uint8_t fix[HEVC_SEI_MAX], host[HEVC_SEI_MAX]; const uint16_t bt2020[6] = {35400, 14600, 8500, 39850, 6550, 2300};
    size_t fn = hevc_sei_build(fix, sizeof fix, bt2020, 15635, 16450, 1000, 50, 1000, 400);
    assert(fn > 0 && ml_engine_get_hdr_test() == 6);                       /* initial: force off, sei and color on */
    hdr_on = 0; color_calls = 0;
    begin(NULL, NULL); mldiag_reset(NULL);                                  /* force off: nothing */
    assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && last_len == pre + sizeof SLICE && ins_count() == 0 && color_calls == 0);
    end();
    assert(ml_engine_set_hdr_test(1, -1, -1) == 7);
    begin(NULL, NULL); mldiag_reset(NULL);                                  /* force on: SEI with the fixed values, colour text */
    assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && last_len == pre + fn + sizeof SLICE && !memcmp(last_pkt + pre, fix, fn));
    assert(ins_count() == 1 && color_calls == 1 && strstr(color_txt, " range:"));
    end();
    assert(ml_engine_set_hdr_test(-1, 0, -1) == 5);                         /* sei off: colour only */
    begin(NULL, NULL); mldiag_reset(NULL); color_calls = 0;
    assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && last_len == pre + sizeof SLICE && color_calls == 1);
    end();
    assert(ml_engine_set_hdr_test(-1, 1, 0) == 3);                          /* colour off: SEI only */
    begin(NULL, NULL); mldiag_reset(NULL); color_calls = 0;
    assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && last_len == pre + fn + sizeof SLICE && color_calls == 0);
    end();
    hdr_on = 1; assert(ml_engine_set_hdr_test(-1, -1, 1) == 7);             /* the host reports HDR: its metadata wins */
    size_t hn = hevc_sei_build(host, sizeof host, (const uint16_t[6]){34000, 16000, 13250, 34500, 7500, 3000}, 15635, 16450, 1000, 50, 800, 400);
    begin(NULL, NULL); mldiag_reset(NULL); color_calls = 0;
    assert(idr_hevc(SLICE, sizeof SLICE) == DR_OK && last_len == pre + hn + sizeof SLICE && !memcmp(last_pkt + pre, host, hn) && color_calls == 1);
    end();
    ml_engine_set_hdr_test(0, 1, 1); }
  vfmt = VIDEO_FORMAT_H264;
}

int ml_engine_full_range(void) { return 0; }

int main(void) {
  sei_test();
  sei_build_test();
  sei_insert_test();
  /* 1: mode off. First frame is 0; a repeated or backwards host PTS gets prev+1. */
  begin(NULL, NULL);
  assert(frame(1000, P) == DR_OK && frame(1016, P) == DR_OK && frame(1016, P) == DR_OK && frame(1005, P) == DR_OK);
  assert(rec_ms[0] == 0 && rec_ms[1] == 16 && rec_ms[2] == 17 && rec_ms[3] == 18 && sub_calls == 4);
  end();
  /* 2: nudge. 10 ms/s: the first 4 ticks measure the baseline lead; ticks at 4, 5 s give a correction of 10, 20 ms. */
  begin("nudge", NULL);
  frame(0, P); frame(1000, P); play_ms = 100;      /* depth 900 */
  for (int i = 1; i <= 3; i++) { esplus_sample_tick(i * 1000); assert(esplus_correction_ms() == 0); }
  esplus_sample_tick(4000); esplus_sample_tick(5000);
  assert(esplus_correction_ms() == 20);
  frame(1100, P); assert(rec_ms[2] == 1100 - 20);
  end();
  /* a correction larger than the relative PTS clamps at 0, then the PTS still rises */
  setenv("MLENGINE_NUDGE_MAX_MS_PER_S", "1000", 1);
  begin("nudge", NULL);
  frame(0, P); frame(2000, P); play_ms = 1;        /* depth 1999, target 100: the cap is above 1 s, the guard holds 1 s */
  for (int i = 1; i <= 5; i++) esplus_sample_tick(i * 1000);
  assert(esplus_correction_ms() > 510);
  frame(510, P); frame(511, P);
  assert(rec_ms[2] == 2001 && rec_ms[3] == 2002);
  end(); unsetenv("MLENGINE_NUDGE_MAX_MS_PER_S");
  /* 2b: env unset + 60 fps: nudge is the default; the sink submits the corrected PTS */
  begin("unset", NULL);
  frame(0, P); frame(1000, P); play_ms = 100;      /* depth 900 */
  for (int i = 1; i <= 5; i++) esplus_sample_tick(i * 1000);
  assert(esplus_lat_mode() == ESPLUS_LAT_NUDGE && esplus_correction_ms() == 20);
  frame(1100, P); assert(rec_ms[2] == 1100 - 20);
  end();
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
  /* 7: a player rebuild restarts the PTS at 0 (the first frame that reaches the PTS code, the IDR); without one nothing changes */
  start_ret = 0; begin(NULL, NULL);
  frame(1000, P); frame(1500, P);                  /* start() fails on the first submit */
  assert(rec_ms[0] == 0 && rec_ms[1] == 500);
  esplus_sample_tick(1000); run_async();           /* start failed -> rebuild; part 2 sets the IDR + rebase flags */
  start_ret = 1; cb_prepared(1);
  assert(frame(9000, P) == DR_NEED_IDR);           /* dropped until an IDR */
  sub_calls = 0;
  assert(frame(9100, FRAME_TYPE_IDR) == DR_OK && rec_ms[0] == 0);   /* fresh timeline: 0, not 8100 */
  assert(frame(9116, P) == DR_OK && rec_ms[1] == 16);
  assert(frame(9132, P) == DR_OK && rec_ms[2] == 32);               /* no second rebase */
  end(); start_ret = 1;
  /* 8: a failed setup (common-c skips cleanup then) closes the player; the next setup makes a new one */
  VidEsplus_FillCallbacks(&cb);
  addv_ok = 0; create_calls = stop_calls = 0;
  assert(cb.setup(VIDEO_FORMAT_H264, 1280, 720, 60, NULL, 0) == -1);
  assert(create_calls == 1 && stop_calls == 1);
  addv_ok = 1;
  assert(cb.setup(VIDEO_FORMAT_H264, 1280, 720, 60, NULL, 0) == 0 && create_calls == 2 && stop_calls == 1);
  end();
  /* 9: the SPS patch. Direct call = the expected bytes. */
  uint8_t exp[H264_SPS_MAX_IN + H264_SPS_EXTRA]; size_t en; h264_sps_info si;
  assert(h264_sps_patch(SPS_NR, sizeof SPS_NR, exp, sizeof exp, &en, &si) == H264_SPS_ADDED && en > sizeof SPS_NR);
  begin(NULL, NULL);
  assert(idr3(SPS_NR, sizeof SPS_NR) == DR_OK && sub_calls == 1);
  check_pkt(exp, en);                              /* patched SPS, PPS and picture untouched */
  { char line[2048]; mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " sps=added/p66/l31/ref") && strstr(line, "/br- | hdr ")); }
  end();
  setenv("MLENGINE_SPS_PATCH", "0", 1); begin(NULL, NULL);   /* off: plain concatenation */
  assert(idr3(SPS_NR, sizeof SPS_NR) == DR_OK);
  check_pkt(SPS_NR, sizeof SPS_NR);
  { char line[2048]; mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " sps=off/p66/l31/ref")); }
  end(); unsetenv("MLENGINE_SPS_PATCH");
  vfmt = VIDEO_FORMAT_H265; hdr_on = 0; begin(NULL, NULL);     /* HEVC: the SPS is never patched (no HDR SEI either) */
  assert(idr3(SPS_NR, sizeof SPS_NR) == DR_OK);
  check_pkt(SPS_NR, sizeof SPS_NR);
  end(); vfmt = VIDEO_FORMAT_H264; hdr_on = 1;
  { static const uint8_t bad[] = { 0, 0, 0, 1, 0x67, 0x42 };   /* truncated SPS */
    begin(NULL, NULL);
    assert(idr3(bad, sizeof bad) == DR_OK);
    check_pkt(bad, sizeof bad);
    char line[2048]; mldiag_fail(-101, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " sps=err | hdr "));
    end(); }
  puts("video esplus ok"); return 0;
}

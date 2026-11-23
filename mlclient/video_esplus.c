/*
 * video_esplus.c - moonlight-common-c DECODER_RENDERER_CALLBACKS backed by the
 * shared Tizen esplusplayer (esplus_player.c). The native equivalent of the WASM
 * fork's wasm/wasmplayer.cpp (EMSS ElementaryMediaTrack.AppendPacket).
 *
 * moonlight-common-c delivers each frame as a chain of LENTRY buffers already in
 * Annex-B; esplusplayer accepts raw Annex-B H.264 (proven by esppplay). We
 * concatenate the chain and submit one ES packet with the frame's PTS.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Limelight.h"
#include "esplus_hdr.h"
#include "esplus_stats.h"
#include "esplusplayer_tv50.h"
#include "h264_sps.h"
#include "hevc_sei.h"
#include "mldiag.h"
#include "mltrace.h"

extern esplusplayer_handle esplus_create_open(void);
extern int esplus_set_window(int w, int h);
extern void esplus_prepare(void);
extern int esplus_add_video_info(const esplusplayer_video_stream_info *vi);
extern int esplus_submit(const esplusplayer_es_packet *pkt);
extern int esplus_submit_ep(const esplusplayer_es_packet *pkt, unsigned epoch);   /* NOT_PREPARED if the epoch is stale */
extern void esplus_close(void);
extern void esplus_set_color_info(const char *txt);   /* HDR colour text for the TV pipeline */
extern int esplus_hdr_force(void);                    /* esplus_player.c: HDR test mode force switch */
extern int ml_engine_full_range(void);               /* engine_api.c: 1 when the session colour range is full */

#define VLOG(...) fprintf(stderr, "[video] " __VA_ARGS__)

static int s_fps;
static int s_h264;   /* the stream is H.264: SPS entries may get a bitstream_restriction */
static int s_hevc;   /* the stream is HEVC: log the SPS once and scan IDR frames for HDR SEI */
static int s_hevc_sps_logged, s_hevc_idr_logged, s_hdr_sei_logged, s_hdr_test_logged;   /* once per session */
static uint8_t *s_scratch;
static size_t s_scratch_cap;
static uint64_t s_base_pts_ms;
static int s_have_base;
static uint64_t s_arrival_base_ms;
static int s_have_arrival;
static int64_t s_prev_pts_ms;
static int s_have_prev;
static uint32_t s_skip_seq;   /* sampler tick of the last late-frame skip */

static int ensure_scratch(size_t need) {
  if (need <= s_scratch_cap) return 0;
  size_t cap = s_scratch_cap ? s_scratch_cap : 65536;
  while (cap < need) cap *= 2;
  uint8_t *p = realloc(s_scratch, cap);
  if (!p) return -1;
  s_scratch = p; s_scratch_cap = cap;
  return 0;
}

/* 1 with the host's HDR metadata when the host is in HDR mode and it is valid; else, with the HDR test force on
   (ml_engine_set_hdr_test), fixed HDR10 test metadata; else 0. */
static int hdr_meta(SS_HDR_METADATA *m) {
  if (LiGetCurrentHostDisplayHdrMode() && LiGetHdrMetadata(m) && m->maxDisplayLuminance > 0) return 1;
  if (!esplus_hdr_force()) return 0;
  memset(m, 0, sizeof *m);
  m->displayPrimaries[0].x = 35400; m->displayPrimaries[0].y = 14600;   /* BT.2020 R, G, B (1/50000) */
  m->displayPrimaries[1].x = 8500;  m->displayPrimaries[1].y = 39850;
  m->displayPrimaries[2].x = 6550;  m->displayPrimaries[2].y = 2300;
  m->whitePoint.x = 15635; m->whitePoint.y = 16450;   /* D65 */
  m->maxDisplayLuminance = 1000; m->minDisplayLuminance = 50; m->maxContentLightLevel = 1000; m->maxFrameAverageLightLevel = 400;
  if (!s_hdr_test_logged) { s_hdr_test_logged = 1;
    VLOG("hdr test: forced, fixed HDR10 metadata lum=1000/50 cll=1000/400\n");
    MLT("hdr test: forced, fixed HDR10 metadata lum=1000/50 cll=1000/400"); }
  return 1;
}

static int VidDecSetup_(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
  (void)context; (void)drFlags;
  MLT("video setup fmt=0x%x %dx%d@%d", videoFormat, width, height, redrawRate);
  s_fps = redrawRate > 0 ? redrawRate : 60;
  s_h264 = (videoFormat & VIDEO_FORMAT_MASK_H264) != 0;
  s_hevc = (videoFormat & VIDEO_FORMAT_MASK_H265) != 0;
  uint32_t mime;
  if (videoFormat & VIDEO_FORMAT_MASK_H264) mime = ESPLUSPLAYER_VIDEO_MIME_TYPE_H264;
  else if (videoFormat & VIDEO_FORMAT_MASK_H265) mime = ESPLUSPLAYER_VIDEO_MIME_TYPE_HEVC;
  else { VLOG("unsupported videoFormat 0x%x\n", videoFormat); MLT("video setup -> -1 (format)"); return -1; }

  if (!esplus_create_open()) { MLT("video setup -> -1 (player)"); return -1; }
  int wrc = esplus_set_window(width, height);
  mldiag_player("win", wrc);
  if (wrc != 0) { MLT("video setup -> -1 (window)"); return -1; }
  esplusplayer_video_stream_info vi = {NULL, 0, mime, (uint32_t)width, (uint32_t)height,
                                       (uint32_t)width, (uint32_t)height, (uint32_t)s_fps, 1};
  int ok = esplus_add_video_info(&vi);   /* also caches it for a player rebuild */
  MLT("esplusplayer_add_video_stream_info mime=%u -> %d", mime, ok);
  if (!ok) { VLOG("add_video_stream_info failed\n"); return -1; }
  VLOG("setup %dx%d@%d fmt=0x%x\n", width, height, s_fps, videoFormat);
  return 0;
}

static void VidDecStart(void) { MLT("video start"); s_base_pts_ms = 0; s_have_base = 0;
  s_hevc_sps_logged = s_hevc_idr_logged = s_hdr_sei_logged = s_hdr_test_logged = 0;
  s_arrival_base_ms = 0; s_have_arrival = 0; s_prev_pts_ms = 0; s_have_prev = 0; s_skip_seq = esplus_sample_seq() - 1;
  esplus_prepare(); }
static void VidDecStop(void) {}
static void VidDecCleanup(void) { MLT("video cleanup"); esplus_close(); free(s_scratch); s_scratch = NULL; s_scratch_cap = 0; }

static int VidDecSetup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
  int rc = VidDecSetup_(videoFormat, width, height, redrawRate, context, drFlags);
  if (rc != 0) esplus_close();   /* common-c skips cleanup when setup fails: do not leave the player open */
  mldiag_video_setup(videoFormat, width, height, redrawRate, rc);
  return rc;
}

static int VidDecSubmit(PDECODE_UNIT du) {
  mldiag_video_du(du->frameType == FRAME_TYPE_IDR);
  unsigned epoch = esplus_epoch();   /* BEFORE the flags: a rebuild that arms the new player after this makes the submit stale */
  if (esplus_take_rebase()) {   /* fresh player after a rebuild: its timeline starts at 0 again, like the first start */
    s_have_base = 0; s_have_prev = 0; s_have_arrival = 0;
    MLT("video: PTS rebase after a player rebuild");
  }
  if (esplus_take_need_idr() && du->frameType != FRAME_TYPE_IDR) return DR_NEED_IDR;   /* after a recovery seek */
  /* experimental, off by default: the sampler's depth only refreshes per tick, so skip at most
     one frame per tick, and never the IDR we asked for. */
  if (esplus_cfg.drop_after_ms > 0 && esplus_stats.depth_ms > esplus_cfg.drop_after_ms
      && du->frameType != FRAME_TYPE_IDR && esplus_sample_seq() != s_skip_seq) {
    s_skip_seq = esplus_sample_seq();
    __atomic_fetch_add(&esplus_stats.dropped_late, 1, __ATOMIC_RELAXED);
    return DR_NEED_IDR;   /* later frames reference this one: common-c drops them and asks for an IDR */
  }
  if (ensure_scratch((size_t)du->fullLength + H264_SPS_EXTRA + HEVC_SEI_MAX)) return DR_NEED_IDR;
  int idr = du->frameType == FRAME_TYPE_IDR, sei_n = 0, sei_mask = 0, pic_seen = 0;   /* the HEVC SEI scan runs on IDR frames only */
  size_t off = 0, grown = 0;   /* grown: bytes the patched SPS entries added so far (never above H264_SPS_EXTRA) */
  uint8_t tmp[H264_SPS_MAX_IN + H264_SPS_EXTRA];   /* function scope: src may point here until the memcpy below */
  for (PLENTRY e = du->bufferList; e; e = e->next) {
    const uint8_t *src = (const uint8_t *)e->data;
    size_t len = (size_t)e->length;
    if (s_h264 && e->bufferType == BUFFER_TYPE_SPS) {
      size_t n = 0; h264_sps_info si;
      int pr = h264_sps_patch((const uint8_t *)e->data, len, tmp, sizeof tmp, &n, &si);
      int st = pr == H264_SPS_ADDED && !esplus_cfg.sps_patch ? MLDIAG_SPS_OFF : pr;
      mldiag_sps(st, si.profile, si.level, si.ref_frames, si.poc_type, si.vui, si.restr, si.reorder, si.dec_buf);
      if (pr == H264_SPS_ADDED && esplus_cfg.sps_patch && n <= len + (H264_SPS_EXTRA - grown)) { src = tmp; if (n > len) grown += n - len; len = n; }
      MLT("sps: %s profile=%d level=%d refs=%d poc=%d vui=%d restr=%d reorder=%d dec_buf=%d len=%d->%d",
          st == MLDIAG_SPS_ADDED ? "added" : st == MLDIAG_SPS_KEPT ? "kept" : st == MLDIAG_SPS_OFF ? "off" : "err",
          si.profile, si.level, si.ref_frames, si.poc_type, si.vui, si.restr, si.reorder, si.dec_buf, e->length, (int)len);
    }
    if (s_hevc && e->bufferType == BUFFER_TYPE_SPS && !s_hevc_sps_logged) {   /* the codec header, no secret */
      char hx[321]; size_t k = len < 160 ? len : 160;
      for (size_t j = 0; j < k; j++) snprintf(hx + 2 * j, 3, "%02x", src[j]);
      hx[2 * k] = 0; s_hevc_sps_logged = 1;
      VLOG("hevc sps: %s\n", hx); MLT("hevc sps: %s", hx);
    }
    if (s_hevc && idr) {
      sei_n += hevc_sei_scan(src, len, &sei_mask);
      SS_HDR_METADATA m;
      int first_pic = e->bufferType == BUFFER_TYPE_PICDATA && !pic_seen++;
      if (first_pic && esplus_cfg.hdr_color && hdr_meta(&m)) {
        uint16_t p[6] = {m.displayPrimaries[0].x, m.displayPrimaries[0].y, m.displayPrimaries[1].x, m.displayPrimaries[1].y,
                         m.displayPrimaries[2].x, m.displayPrimaries[2].y};
        char txt[512];
        if (hdr_color_text(txt, sizeof txt, p, m.whitePoint.x, m.whitePoint.y, m.maxDisplayLuminance, m.minDisplayLuminance,
                           m.maxContentLightLevel, m.maxFrameAverageLightLevel, ml_engine_full_range())) esplus_set_color_info(txt);
      }
      /* the TV decoder only reads HDR metadata from the stream: add it before the slices when the host sent none */
      if (first_pic && !(sei_mask & 1) && esplus_cfg.hdr_sei && hdr_meta(&m)) {
        uint16_t p[6] = {m.displayPrimaries[0].x, m.displayPrimaries[0].y, m.displayPrimaries[1].x, m.displayPrimaries[1].y,
                         m.displayPrimaries[2].x, m.displayPrimaries[2].y};
        size_t k = hevc_sei_build(s_scratch + off, s_scratch_cap - off, p, m.whitePoint.x, m.whitePoint.y, m.maxDisplayLuminance,
                                  m.minDisplayLuminance, m.maxContentLightLevel, m.maxFrameAverageLightLevel);
        if (k) {
          off += k; mldiag_hdr_sei_inserted();
          if (!s_hdr_sei_logged) { s_hdr_sei_logged = 1;
            VLOG("hevc hdr sei inserted bytes=%zu lum=%u/%u cll=%u/%u\n", k, m.maxDisplayLuminance, m.minDisplayLuminance, m.maxContentLightLevel, m.maxFrameAverageLightLevel);
            MLT("hevc hdr sei inserted bytes=%zu lum=%u/%u cll=%u/%u", k, m.maxDisplayLuminance, m.minDisplayLuminance, m.maxContentLightLevel, m.maxFrameAverageLightLevel); }
        }
      }
    }
    memcpy(s_scratch + off, src, len);
    off += len;
  }
  if (s_hevc && idr) {
    mldiag_hevc_sei(sei_n, sei_mask);
    if (!s_hevc_idr_logged) { s_hevc_idr_logged = 1; VLOG("hevc idr sei=%d mask=0x%x\n", sei_n, sei_mask); MLT("hevc idr sei=%d mask=0x%x", sei_n, sei_mask); }
  }
  if (!s_have_base) { s_base_pts_ms = du->presentationTimeMs; s_have_base = 1; }
  int64_t pts_ms;
  int mode = esplus_lat_mode();
  if (mode == ESPLUS_LAT_ARRIVAL) {
    uint64_t now = esplus_mono_ms();
    if (!s_have_arrival) { s_arrival_base_ms = now; s_have_arrival = 1; }
    pts_ms = (int64_t)(now - s_arrival_base_ms);
  } else {
    pts_ms = (int64_t)(du->presentationTimeMs - s_base_pts_ms);
    if (mode == ESPLUS_LAT_NUDGE) pts_ms -= esplus_correction_ms();
  }
  if (pts_ms < 0) pts_ms = 0;
  /* the player's PTS resolution is 1 ms: keep PTS strictly increasing */
  if (s_have_prev && pts_ms <= s_prev_pts_ms) pts_ms = s_prev_pts_ms + 1;
  s_prev_pts_ms = pts_ms; s_have_prev = 1;
  esplusplayer_es_packet pkt = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, (char *)s_scratch, (uint32_t)off,
                                (uint64_t)pts_ms * 1000000ull, (1000000000ull / s_fps)};
  int r = esplus_submit_ep(&pkt, epoch);
  /* FULL backpressure: the player queue is full; retry briefly, then drop the frame (DR_NEED_IDR) */
  for (int i = 0; r == ESPLUSPLAYER_SUBMIT_STATUS_FULL && i < 3; i++) { usleep(2000); r = esplus_submit_ep(&pkt, epoch); }
  if (r == ESPLUSPLAYER_SUBMIT_STATUS_FULL) {
    __atomic_fetch_add(&esplus_stats.dropped_full, 1, __ATOMIC_RELAXED);
    return DR_NEED_IDR;
  }
  if (r == ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS || r == ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED)
    return DR_OK;
  VLOG("submit_packet failed %d\n", r);
  return DR_NEED_IDR;
}

void VidEsplus_FillCallbacks(PDECODER_RENDERER_CALLBACKS cb) {
  LiInitializeVideoCallbacks(cb);
  cb->setup = VidDecSetup;
  cb->start = VidDecStart;
  cb->stop = VidDecStop;
  cb->cleanup = VidDecCleanup;
  cb->submitDecodeUnit = VidDecSubmit;
  cb->capabilities = CAPABILITY_DIRECT_SUBMIT;
}

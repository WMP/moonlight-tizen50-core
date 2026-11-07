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
#include "esplus_stats.h"
#include "esplusplayer_tv50.h"
#include "mldiag.h"
#include "mltrace.h"

extern esplusplayer_handle esplus_create_open(void);
extern esplusplayer_handle esplus_handle(void);
extern int esplus_set_window(int w, int h);
extern void esplus_prepare(void);
extern int esplus_add_video_info(const esplusplayer_video_stream_info *vi);
extern int esplus_submit(const esplusplayer_es_packet *pkt);
extern void esplus_close(void);

#define VLOG(...) fprintf(stderr, "[video] " __VA_ARGS__)

static int s_fps;
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

static int VidDecSetup_(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
  (void)context; (void)drFlags;
  MLT("video setup fmt=0x%x %dx%d@%d", videoFormat, width, height, redrawRate);
  s_fps = redrawRate > 0 ? redrawRate : 60;
  uint32_t mime;
  if (videoFormat & VIDEO_FORMAT_MASK_H264) mime = ESPLUSPLAYER_VIDEO_MIME_TYPE_H264;
  else if (videoFormat & VIDEO_FORMAT_MASK_H265) mime = ESPLUSPLAYER_VIDEO_MIME_TYPE_HEVC;
  else { VLOG("unsupported videoFormat 0x%x\n", videoFormat); MLT("video setup -> -1 (format)"); return -1; }

  if (!esplus_create_open()) { MLT("video setup -> -1 (player)"); return -1; }   /* audio init may have created it */
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
  s_arrival_base_ms = 0; s_have_arrival = 0; s_prev_pts_ms = 0; s_have_prev = 0; s_skip_seq = esplus_sample_seq() - 1;
  esplus_prepare(); }
static void VidDecStop(void) {}
static void VidDecCleanup(void) { MLT("video cleanup"); esplus_close(); free(s_scratch); s_scratch = NULL; s_scratch_cap = 0; }

static int VidDecSetup(int videoFormat, int width, int height, int redrawRate, void *context, int drFlags) {
  int rc = VidDecSetup_(videoFormat, width, height, redrawRate, context, drFlags);
  mldiag_video_setup(videoFormat, width, height, redrawRate, rc);
  return rc;
}

static int VidDecSubmit(PDECODE_UNIT du) {
  mldiag_video_du(du->frameType == FRAME_TYPE_IDR);
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
  if (ensure_scratch((size_t)du->fullLength)) return DR_NEED_IDR;
  size_t off = 0;
  for (PLENTRY e = du->bufferList; e; e = e->next) {
    memcpy(s_scratch + off, e->data, (size_t)e->length);
    off += (size_t)e->length;
  }
  if (!s_have_base) { s_base_pts_ms = du->presentationTimeMs; s_have_base = 1; }
  int64_t pts_ms;
  if (esplus_cfg.latency_mode == ESPLUS_LAT_ARRIVAL) {
    uint64_t now = esplus_mono_ms();
    if (!s_have_arrival) { s_arrival_base_ms = now; s_have_arrival = 1; }
    pts_ms = (int64_t)(now - s_arrival_base_ms);
  } else {
    pts_ms = (int64_t)(du->presentationTimeMs - s_base_pts_ms);
    if (esplus_cfg.latency_mode == ESPLUS_LAT_NUDGE) pts_ms -= esplus_correction_ms();
  }
  if (pts_ms < 0) pts_ms = 0;
  /* the player's PTS resolution is 1 ms: keep PTS strictly increasing */
  if (s_have_prev && pts_ms <= s_prev_pts_ms) pts_ms = s_prev_pts_ms + 1;
  s_prev_pts_ms = pts_ms; s_have_prev = 1;
  esplusplayer_es_packet pkt = {ESPLUSPLAYER_STREAM_TYPE_VIDEO, (char *)s_scratch, (uint32_t)off,
                                (uint64_t)pts_ms * 1000000ull, (1000000000ull / s_fps)};
  int r = esplus_submit(&pkt);
  /* FULL backpressure: the player queue is full; retry briefly, then drop the frame (DR_NEED_IDR) */
  for (int i = 0; r == ESPLUSPLAYER_SUBMIT_STATUS_FULL && i < 3; i++) { usleep(2000); r = esplus_submit(&pkt); }
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

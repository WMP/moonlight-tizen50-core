/*
 * audio_opus.c - moonlight-common-c AUDIO_RENDERER_CALLBACKS backed by the shared
 * esplusplayer (esplus_player.c). Submits the host's Opus frames straight to an
 * audio/x-opus track on the same player as video, so audio and video share one
 * clock (tight A/V sync) - the native equivalent of the fork's EMSS audio track.
 * No libopus decode step: esplusplayer/GStreamer decodes Opus internally.
 *
 * codec_data is a standard RFC 7845 OpusHead built from the negotiated
 * OPUS_MULTISTREAM_CONFIGURATION.
 *
 * UNTESTED: whether esplusplayer wants the OpusHead exactly like this as
 * codec_data is not yet confirmed on the TV. If it rejects it, the fallback is
 * to decode with libopus (vendored) and submit audio/x-raw PCM, or route PCM to
 * PulseAudio. The audio mime value OPUS=8 and the stream-info struct ARE
 * confirmed from the .so (see esplusplayer_tv50.h).
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "Limelight.h"
#include "esplusplayer_tv50.h"
#include "mltrace.h"

extern esplusplayer_handle esplus_create_open(void);
extern esplusplayer_handle esplus_handle(void);
extern void esplus_prepare(void);
extern int esplus_submit(const esplusplayer_es_packet *pkt);

#define ALOG(...) fprintf(stderr, "[audio] " __VA_ARGS__)

static uint8_t s_opushead[32];
static int s_opushead_len;
static int s_sample_rate = 48000, s_samples_per_frame = 240;
static uint64_t s_pts_ns;

/* RFC 7845 OpusHead. Returns length. */
static int build_opushead(uint8_t *o, const POPUS_MULTISTREAM_CONFIGURATION c) {
  int n = 0;
  memcpy(o, "OpusHead", 8); n = 8;
  o[n++] = 1;                               /* version */
  o[n++] = (uint8_t)c->channelCount;        /* channel count */
  uint16_t preskip = 3840;                  /* standard 80 ms at 48 kHz */
  o[n++] = preskip & 0xff; o[n++] = preskip >> 8;
  uint32_t sr = (uint32_t)c->sampleRate;    /* input sample rate */
  o[n++] = sr & 0xff; o[n++] = (sr >> 8) & 0xff; o[n++] = (sr >> 16) & 0xff; o[n++] = (sr >> 24) & 0xff;
  o[n++] = 0; o[n++] = 0;                    /* output gain */
  if (c->channelCount <= 2 && c->streams == 1) {
    o[n++] = 0;                             /* mapping family 0 */
  } else {
    o[n++] = 1;                             /* mapping family 1 (multichannel) */
    o[n++] = (uint8_t)c->streams;
    o[n++] = (uint8_t)c->coupledStreams;
    for (int i = 0; i < c->channelCount; i++) o[n++] = c->mapping[i];
  }
  return n;
}

static int ar_init(int audioConfig, const POPUS_MULTISTREAM_CONFIGURATION cfg, void *ctx, int flags) {
  (void)audioConfig; (void)ctx; (void)flags;
  MLT("audio init cfg=0x%x %d Hz %d ch streams=%d", audioConfig, cfg ? cfg->sampleRate : 0,
      cfg ? cfg->channelCount : 0, cfg ? cfg->streams : 0);
  if (!cfg || cfg->channelCount <= 0 || cfg->channelCount > 8 || cfg->sampleRate <= 0) { MLT("audio init -> -1 (config)"); return -1; }
  s_sample_rate = cfg->sampleRate;
  s_samples_per_frame = cfg->samplesPerFrame;
  s_pts_ns = 0;
  s_opushead_len = build_opushead(s_opushead, cfg);

  if (!esplus_create_open()) { ALOG("player create failed\n"); MLT("audio init -> -1 (player)"); return -1; }
  esplusplayer_audio_stream_info ai = {
    (char *)s_opushead, (uint32_t)s_opushead_len, ESPLUSPLAYER_AUDIO_MIME_TYPE_OPUS,
    0, (uint32_t)cfg->channelCount, (uint32_t)cfg->sampleRate};
  int ok = esplusplayer_add_audio_stream_info(esplus_handle(), &ai);
  MLT("esplusplayer_add_audio_stream_info opus -> %d", ok);
  if (!ok) { ALOG("add_audio_stream_info failed\n"); return -1; }
  ALOG("init %d Hz, %d ch, %d samples/frame, OpusHead %d B\n",
       cfg->sampleRate, cfg->channelCount, cfg->samplesPerFrame, s_opushead_len);
  return 0;
}

static void ar_start(void) { esplus_prepare(); }   /* prepare once video has started too */
static void ar_stop(void) {}
static void ar_cleanup(void) {}

static void ar_decodePlay(char *data, int len) {
  if (len <= 0) return;
  uint64_t dur_ns = (uint64_t)s_samples_per_frame * 1000000000ull / (uint64_t)s_sample_rate;
  esplusplayer_es_packet pkt = {ESPLUSPLAYER_STREAM_TYPE_AUDIO, data, (uint32_t)len, s_pts_ns, dur_ns};
  int r = esplus_submit(&pkt);
  if (r != ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS && r != ESPLUSPLAYER_SUBMIT_STATUS_FULL
      && r != ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED)
    ALOG("submit audio failed %d\n", r);
  /* NOT_PREPARED = the packet was not queued (audio waits for the first video packet): the audio PTS starts at 0 with it;
     FULL and other failures lose their time slot, so they still advance */
  if (r != ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED) s_pts_ns += dur_ns;
}

void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb) {
  LiInitializeAudioCallbacks(cb);
  cb->init = ar_init;
  cb->start = ar_start;
  cb->stop = ar_stop;
  cb->cleanup = ar_cleanup;
  cb->decodeAndPlaySample = ar_decodePlay;
  cb->capabilities = CAPABILITY_DIRECT_SUBMIT;
}

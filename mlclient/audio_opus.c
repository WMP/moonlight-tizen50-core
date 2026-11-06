/*
 * audio_opus.c - moonlight-common-c AUDIO_RENDERER_CALLBACKS backed by the shared
 * esplusplayer (esplus_player.c). The host's Opus frames are decoded in the engine
 * with libopus (fixed point, from the submodule; libopus_tv.a) to S16LE PCM, which is
 * submitted as audio/x-raw to the same player as video, so audio and video share one clock.
 *
 * The TV player cannot decode Opus (prepare fails right after the first audio buffer).
 * The PCM mime value 9 (ESPLUSPLAYER_AUDIO_MIME_TYPE_PCM_S16LE) is ASSUMED, not yet
 * validated on the TV (see esplusplayer_tv50.h).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus_multistream.h>

#include "Limelight.h"
#include "esplusplayer_tv50.h"
#include "mldiag.h"
#include "mltrace.h"

extern esplusplayer_handle esplus_create_open(void);
extern esplusplayer_handle esplus_handle(void);
extern void esplus_prepare(void);
extern int esplus_submit(const esplusplayer_es_packet *pkt);

#define ALOG(...) fprintf(stderr, "[audio] " __VA_ARGS__)

static int s_sample_rate = 48000, s_samples_per_frame = 240, s_channels = 2;
static uint64_t s_pts_ns;
static OpusMSDecoder *s_dec;
static opus_int16 *s_pcm;   /* one frame; esplusplayer copies the packet buffer, so one buffer is reused */
static unsigned s_nerr;

static void ar_cleanup(void) {
  if (s_dec) opus_multistream_decoder_destroy(s_dec);
  free(s_pcm);
  s_dec = NULL; s_pcm = NULL;
}

static int ar_init(int audioConfig, const POPUS_MULTISTREAM_CONFIGURATION cfg, void *ctx, int flags) {
  (void)audioConfig; (void)ctx; (void)flags;
  MLT("audio init cfg=0x%x %d Hz %d ch streams=%d", audioConfig, cfg ? cfg->sampleRate : 0,
      cfg ? cfg->channelCount : 0, cfg ? cfg->streams : 0);
  if (!cfg || cfg->channelCount <= 0 || cfg->channelCount > 8 || cfg->sampleRate <= 0 || cfg->samplesPerFrame <= 0) { MLT("audio init -> -1 (config)"); return -1; }
  ar_cleanup();   /* a decoder left over from a previous session */
  s_sample_rate = cfg->sampleRate;
  s_samples_per_frame = cfg->samplesPerFrame;
  s_channels = cfg->channelCount;
  s_pts_ns = 0; s_nerr = 0;

  int err = 0;
  s_dec = opus_multistream_decoder_create(cfg->sampleRate, cfg->channelCount, cfg->streams, cfg->coupledStreams, cfg->mapping, &err);
  if (!s_dec) {
    ALOG("opus decoder create failed %d\n", err); MLT("audio init -> -1 (opus %d)", err);
    mldiag_adec(0, err);
    return -1;
  }
  s_pcm = malloc((size_t)s_samples_per_frame * (size_t)s_channels * sizeof *s_pcm);
  if (!s_pcm) { ar_cleanup(); ALOG("out of memory\n"); return -1; }

  if (!esplus_create_open()) { ALOG("player create failed\n"); MLT("audio init -> -1 (player)"); return -1; }
  esplusplayer_audio_stream_info ai = {
    NULL, 0, ESPLUSPLAYER_AUDIO_MIME_TYPE_PCM_S16LE,
    0, (uint32_t)cfg->channelCount, (uint32_t)cfg->sampleRate};
  mldiag_audio_mime((int)ai.mime_type);
  int ok = esplusplayer_add_audio_stream_info(esplus_handle(), &ai);
  MLT("esplusplayer_add_audio_stream_info pcm -> %d", ok);
  if (!ok) { ALOG("add_audio_stream_info failed\n"); return -1; }
  ALOG("init %d Hz, %d ch, %d samples/frame, PCM S16LE\n", cfg->sampleRate, cfg->channelCount, cfg->samplesPerFrame);
  return 0;
}

static void ar_start(void) { esplus_prepare(); }   /* prepare once video has started too */
static void ar_stop(void) {}

static void ar_decodePlay(char *data, int len) {
  if (!s_dec || !s_pcm) return;
  /* no data = a lost packet: libopus conceals it */
  int n = (data == NULL || len <= 0)
      ? opus_multistream_decode(s_dec, NULL, 0, s_pcm, s_samples_per_frame, 0)
      : opus_multistream_decode(s_dec, (const unsigned char *)data, len, s_pcm, s_samples_per_frame, 0);
  if (n <= 0) {
    mldiag_adec(0, n);
    if (s_nerr++ < 4 || s_nerr % 256 == 0) MLT("opus decode error %d (#%u)", n, s_nerr);
    /* that time slot is lost: the PTS still advances */
    s_pts_ns += (uint64_t)s_samples_per_frame * 1000000000ull / (uint64_t)s_sample_rate;
    return;
  }
  mldiag_adec(1, 0);
  uint64_t dur_ns = (uint64_t)n * 1000000000ull / (uint64_t)s_sample_rate;
  esplusplayer_es_packet pkt = {ESPLUSPLAYER_STREAM_TYPE_AUDIO, (char *)s_pcm, (uint32_t)n * (uint32_t)s_channels * 2u, s_pts_ns, dur_ns};
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
  /* no CAPABILITY_DIRECT_SUBMIT: moonlight-common-c queues the packets and calls decodeAndPlaySample from its
     own audio decoder thread, so libopus never runs on the receive thread */
  cb->capabilities = 0;
}

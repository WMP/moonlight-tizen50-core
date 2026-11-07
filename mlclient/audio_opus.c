/*
 * audio_opus.c - moonlight-common-c AUDIO_RENDERER_CALLBACKS. The host's Opus frames are
 * decoded in the engine with libopus (fixed point, from the submodule; libopus_tv.a) to S16LE
 * PCM, which audio_pulse.c writes to the TV's PulseAudio.
 *
 * Platform limitation: the TV player rejects engine-fed audio streams (both Opus and raw PCM),
 * so the esplusplayer carries only video (esplus_player.c) and audio does not share its clock.
 * If PulseAudio is not available the stream goes on without sound: init still returns 0 and
 * decodeAndPlaySample returns at once (no decode: nothing could play it).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <opus_multistream.h>

#include "Limelight.h"
#include "audio_pulse.h"
#include "mldiag.h"
#include "mltrace.h"

#define ALOG(...) fprintf(stderr, "[audio] " __VA_ARGS__)

static int s_samples_per_frame = 240, s_channels = 2;
static OpusMSDecoder *s_dec;
static opus_int16 *s_pcm;   /* one frame, reused: audio_pulse_write copies it into the server buffer */
static int s_out;           /* 1 = PulseAudio is open */
static unsigned s_nerr;

static int env_int(const char *name, int def) {   /* same rule as the other knobs: a non-negative number */
  const char *v = getenv(name);
  char *end;
  if (!v || !*v) return def;
  long n = strtol(v, &end, 10);
  return *end || n < 0 ? def : (int)n;
}

static void ar_cleanup(void) {
  audio_pulse_close();
  if (s_dec) opus_multistream_decoder_destroy(s_dec);
  free(s_pcm);
  s_dec = NULL; s_pcm = NULL; s_out = 0;
}

static int ar_init(int audioConfig, const POPUS_MULTISTREAM_CONFIGURATION cfg, void *ctx, int flags) {
  (void)audioConfig; (void)ctx; (void)flags;
  MLT("audio init cfg=0x%x %d Hz %d ch streams=%d", audioConfig, cfg ? cfg->sampleRate : 0,
      cfg ? cfg->channelCount : 0, cfg ? cfg->streams : 0);
  if (!cfg || cfg->channelCount <= 0 || cfg->channelCount > 8 || cfg->sampleRate <= 0 || cfg->samplesPerFrame <= 0) { MLT("audio init -> -1 (config)"); return -1; }
  ar_cleanup();   /* a decoder left over from a previous session */
  s_samples_per_frame = cfg->samplesPerFrame;
  s_channels = cfg->channelCount;
  s_nerr = 0;

  int err = 0;
  s_dec = opus_multistream_decoder_create(cfg->sampleRate, cfg->channelCount, cfg->streams, cfg->coupledStreams, cfg->mapping, &err);
  if (!s_dec) {
    ALOG("opus decoder create failed %d\n", err); MLT("audio init -> -1 (opus %d)", err);
    mldiag_adec(0, err);
    return -1;
  }
  s_pcm = malloc((size_t)s_samples_per_frame * (size_t)s_channels * sizeof *s_pcm);
  if (!s_pcm) { ar_cleanup(); ALOG("out of memory\n"); return -1; }

  s_out = audio_pulse_open(cfg->sampleRate, cfg->channelCount, env_int("MLENGINE_AUDIO_LATENCY_MS", 40)) == 0;
  if (!s_out) { ALOG("audio output unavailable (pulse)\n"); MLT("audio output unavailable (pulse)"); }   /* the reason is in mldiag */
  ALOG("init %d Hz, %d ch, %d samples/frame, PCM S16LE -> PulseAudio %s\n", cfg->sampleRate, cfg->channelCount, cfg->samplesPerFrame, s_out ? "ok" : "unavailable");
  return 0;
}

static void ar_start(void) {}
static void ar_stop(void) { audio_pulse_drain(); }

static void ar_decodePlay(char *data, int len) {
  if (!s_dec || !s_pcm || !s_out) return;
  /* no data = a lost packet: libopus conceals it */
  int n = (data == NULL || len <= 0)
      ? opus_multistream_decode(s_dec, NULL, 0, s_pcm, s_samples_per_frame, 0)
      : opus_multistream_decode(s_dec, (const unsigned char *)data, len, s_pcm, s_samples_per_frame, 0);
  if (n <= 0) {
    mldiag_adec(0, n);
    if (s_nerr++ < 4 || s_nerr % 256 == 0) MLT("opus decode error %d (#%u)", n, s_nerr);
    return;   /* nothing is written: PulseAudio plays silence for the lost slot */
  }
  mldiag_adec(1, 0);
  /* blocks while the PulseAudio buffer is full: the back pressure (we run on common-c's audio thread) */
  audio_pulse_write(s_pcm, (size_t)n * (size_t)s_channels * 2u);
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

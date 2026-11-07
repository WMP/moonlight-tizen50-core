/*
 * Host self-test for mlclient/audio_opus.c (Opus -> PCM S16LE -> audio_pulse_write), driven through
 * AudEsplus_FillCallbacks. audio_pulse is stubbed; libopus encodes the test frames.
 * Run: sh tests/build-esplus-ecore-test.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus_multistream.h>
#include "Limelight.h"
#include "audio_pulse.h"
#include "mldiag.h"

void LiInitializeAudioCallbacks(PAUDIO_RENDERER_CALLBACKS c) { memset(c, 0, sizeof *c); }
void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb);

/* audio_pulse stub: records what audio_opus.c writes */
static int n_open, n_close, n_drain, n_write, open_rc, open_rate, open_ch, open_lat;
static size_t sizes[16];
int audio_pulse_open(int rate, int channels, int latency_ms) { n_open++; open_rate = rate; open_ch = channels; open_lat = latency_ms; return open_rc; }
int audio_pulse_write(const void *pcm, size_t bytes) { assert(pcm); if (n_write < 16) sizes[n_write] = bytes; n_write++; return 0; }
void audio_pulse_drain(void) { n_drain++; }
void audio_pulse_close(void) { n_close++; }

#define MS 1000000ull
enum { SPF = 240 };

int main(void) {
  OPUS_MULTISTREAM_CONFIGURATION cfg;
  memset(&cfg, 0, sizeof cfg);
  cfg.sampleRate = 48000; cfg.channelCount = 2; cfg.streams = 1; cfg.coupledStreams = 1;
  cfg.samplesPerFrame = SPF; cfg.mapping[0] = 0; cfg.mapping[1] = 1;

  int err = 0;
  OpusMSEncoder *enc = opus_multistream_encoder_create(48000, 2, 1, 1, cfg.mapping, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
  assert(enc && err == OPUS_OK);
  unsigned char fr[4][400]; int fl[4];
  for (int k = 0; k < 4; k++) {
    opus_int16 pcm[SPF * 2];
    for (int i = 0; i < SPF; i++) { pcm[2 * i] = (opus_int16)(((i + k * SPF) * 300) % 20000); pcm[2 * i + 1] = (opus_int16)-pcm[2 * i]; }
    fl[k] = opus_multistream_encode(enc, pcm, SPF, fr[k], sizeof fr[k]);
    assert(fl[k] > 0);
  }
  opus_multistream_encoder_destroy(enc);

  AUDIO_RENDERER_CALLBACKS cb;
  AudEsplus_FillCallbacks(&cb);
  assert(!(cb.capabilities & CAPABILITY_DIRECT_SUBMIT));

  for (int round = 0; round < 2; round++) {   /* round 1: cleanup, then a second init */
    mldiag_reset(NULL);
    n_write = 0; open_rc = 0; n_open = 0;
    assert(cb.init(0, &cfg, NULL, 0) == 0);
    assert(n_open == 1 && open_rate == 48000 && open_ch == 2 && open_lat == 40);   /* MLENGINE_AUDIO_LATENCY_MS default */
    cb.start();

    for (int k = 0; k < 3; k++) cb.decodeAndPlaySample((char *)fr[k], fl[k]);
    assert(n_write == 3);
    for (int k = 0; k < 3; k++) assert(sizes[k] == SPF * 2 * 2);   /* 960 B per 5 ms frame */

    cb.decodeAndPlaySample(NULL, 0);   /* loss: concealed frame */
    assert(n_write == 4 && sizes[3] == SPF * 2 * 2);
    cb.decodeAndPlaySample((char *)fr[0], 0);   /* len 0 with data: also a loss */
    assert(n_write == 5 && sizes[4] == SPF * 2 * 2);

    char junk[3] = { (char)0xff, (char)0xff, (char)0xff };   /* undecodable: counted, nothing written */
    cb.decodeAndPlaySample(junk, (int)sizeof junk);
    assert(n_write == 5);
    cb.decodeAndPlaySample((char *)fr[3], fl[3]);
    assert(n_write == 6);

    char line[4096];
    mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " adec=6 adec_fail=1/-4 err="));   /* -4 = OPUS_INVALID_PACKET */

    n_drain = 0; cb.stop(); assert(n_drain == 1);
    n_close = 0; cb.cleanup(); assert(n_close >= 1);
    cb.decodeAndPlaySample((char *)fr[0], fl[0]);   /* no decoder: ignored */
    assert(n_write == 6);
  }

  /* no PulseAudio: init still succeeds, decode returns early (nothing is written, nothing crashes) */
  open_rc = -1; n_write = 0;
  assert(cb.init(0, &cfg, NULL, 0) == 0);
  cb.start(); cb.decodeAndPlaySample((char *)fr[0], fl[0]); cb.decodeAndPlaySample(NULL, 0); cb.stop();
  assert(n_write == 0);
  cb.cleanup();
  setenv("MLENGINE_AUDIO_LATENCY_MS", "120", 1); open_rc = 0;
  assert(cb.init(0, &cfg, NULL, 0) == 0 && open_lat == 120);
  cb.cleanup(); unsetenv("MLENGINE_AUDIO_LATENCY_MS");

  cfg.channelCount = 0;
  assert(cb.init(0, &cfg, NULL, 0) == -1);
  puts("audio_opus_test OK");
  return 0;
}

/*
 * Host self-test for mlclient/audio_opus.c (Opus -> PCM S16LE), driven through
 * AudEsplus_FillCallbacks. The player is stubbed; libopus encodes the test frames.
 * Run: sh tests/build-esplus-ecore-test.sh
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <opus_multistream.h>
#include "Limelight.h"
#include "esplusplayer_tv50.h"
#include "mldiag.h"

void LiInitializeAudioCallbacks(PAUDIO_RENDERER_CALLBACKS c) { memset(c, 0, sizeof *c); }
void AudEsplus_FillCallbacks(PAUDIO_RENDERER_CALLBACKS cb);

static int h, n_open, n_prepare, n_submit, submit_rc = ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS;
static esplusplayer_audio_stream_info info;
static esplusplayer_es_packet last_pkt, pkts[16];
esplusplayer_handle esplus_create_open(void) { n_open++; return (esplusplayer_handle)&h; }
esplusplayer_handle esplus_handle(void) { return (esplusplayer_handle)&h; }
void esplus_prepare(void) { n_prepare++; }
int esplusplayer_add_audio_stream_info(esplusplayer_handle p, const esplusplayer_audio_stream_info *i) { (void)p; info = *i; return 1; }
int esplus_submit(const esplusplayer_es_packet *p) {
  last_pkt = *p;
  if (n_submit < 16) pkts[n_submit] = *p;
  n_submit++;
  return submit_rc;
}

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
    n_submit = 0; submit_rc = ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS;
    assert(cb.init(0, &cfg, NULL, 0) == 0);
    assert(info.mime_type == ESPLUSPLAYER_AUDIO_MIME_TYPE_PCM_S16LE && info.mime_type == 9);
    assert(info.codec_data == NULL && info.codec_data_length == 0 && info.bitrate == 0);
    assert(info.channels == 2 && info.sample_rate == 48000);
    cb.start(); assert(n_prepare == round + 1);

    for (int k = 0; k < 3; k++) cb.decodeAndPlaySample((char *)fr[k], fl[k]);
    assert(n_submit == 3);
    for (int k = 0; k < 3; k++) {
      assert(pkts[k].type == ESPLUSPLAYER_STREAM_TYPE_AUDIO && pkts[k].buffer_size == SPF * 2 * 2);
      assert(pkts[k].duration == 5 * MS && pkts[k].pts == (unsigned long long)k * 5 * MS);
    }

    cb.decodeAndPlaySample(NULL, 0);   /* loss: concealed frame */
    assert(n_submit == 4 && last_pkt.buffer_size == SPF * 2 * 2 && last_pkt.pts == 15 * MS && last_pkt.duration == 5 * MS);

    char junk[3] = { (char)0xff, (char)0xff, (char)0xff };   /* undecodable: counted, no submit, PTS advances */
    cb.decodeAndPlaySample(junk, 0 + (int)sizeof junk);
    assert(n_submit == 4);
    cb.decodeAndPlaySample((char *)fr[3], fl[3]);
    assert(n_submit == 5 && last_pkt.pts == 25 * MS);   /* 20 ms after the concealed one + 5 ms lost slot */

    char line[4096];
    mldiag_fail(-1, -1, NULL, 0, 0, 0, 0, 0, 0, 0, 0); mldiag_last(line, sizeof line);
    assert(strstr(line, " amime=9 adec=5 adec_fail=1/-4 err="));   /* -4 = OPUS_INVALID_PACKET */

    submit_rc = ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED;   /* refused: PTS must not advance */
    cb.decodeAndPlaySample((char *)fr[0], fl[0]);
    cb.decodeAndPlaySample((char *)fr[1], fl[1]);
    assert(n_submit == 7 && pkts[5].pts == 30 * MS && pkts[6].pts == 30 * MS);
    submit_rc = ESPLUSPLAYER_SUBMIT_STATUS_FULL;           /* lost slot: advances */
    cb.decodeAndPlaySample((char *)fr[2], fl[2]);
    cb.decodeAndPlaySample((char *)fr[3], fl[3]);
    assert(n_submit == 9 && last_pkt.pts == 35 * MS);

    cb.cleanup();
    cb.decodeAndPlaySample((char *)fr[0], fl[0]);   /* no decoder: ignored */
    assert(n_submit == 9);
  }

  cfg.channelCount = 0;
  assert(cb.init(0, &cfg, NULL, 0) == -1);
  puts("audio_opus_test OK");
  return 0;
}

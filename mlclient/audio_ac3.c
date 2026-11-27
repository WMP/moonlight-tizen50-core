#include "audio_ac3.h"

#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/samplefmt.h>

#include "esplus_stats.h"
#include "esplusplayer_tv50.h"
#include "mldiag.h"

enum { CHANNELS = 6, RATE = 48000, FRAME_SAMPLES = 1536, FRAME_MS = 32,
       BITRATE = 448000, FRAME_BYTES = 1792, QUEUE_FRAMES = 6 };
enum { LEGACY, WAITING, ACTIVE };

extern int esplus_submit_ep(const esplusplayer_es_packet *pkt, unsigned epoch);

static AVCodecContext *encoder;
static AVFrame *pcm;
static AVPacket *packet;
static int samples;
static int mode;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char queue[QUEUE_FRAMES][FRAME_BYTES];
static unsigned read_at, queued;
static uint64_t next_ms;
static unsigned timeline_epoch;
static int timeline_valid;

void ml_ac3_select(int enabled) {
  __atomic_store_n(&mode, enabled ? WAITING : LEGACY, __ATOMIC_RELEASE);
}

int ml_ac3_waiting(void) {
  return __atomic_load_n(&mode, __ATOMIC_ACQUIRE) == WAITING;
}

void ml_ac3_ready(int active) {
  __atomic_store_n(&mode, active ? ACTIVE : LEGACY, __ATOMIC_RELEASE);
}

void ml_ac3_close(void) {
  pthread_mutex_lock(&queue_lock);
  read_at = queued = 0;
  next_ms = 0;
  timeline_valid = 0;
  pthread_mutex_unlock(&queue_lock);
  samples = 0;
  av_packet_free(&packet);
  av_frame_free(&pcm);
  avcodec_free_context(&encoder);
}

int ml_ac3_open(int rate, int channels) {
  if (rate != RATE || channels != CHANNELS) return 0;
  const AVCodec *codec = avcodec_find_encoder_by_name("ac3_fixed");
  if (!codec) return 0;
  encoder = avcodec_alloc_context3(codec);
  pcm = av_frame_alloc();
  packet = av_packet_alloc();
  if (!encoder || !pcm || !packet) goto failed;
  encoder->sample_fmt = AV_SAMPLE_FMT_S32P;
  encoder->sample_rate = RATE;
  encoder->time_base = (AVRational){1, RATE};
  encoder->bit_rate = BITRATE;
  encoder->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT1;
  if (avcodec_open2(encoder, codec, NULL) < 0 || encoder->frame_size != FRAME_SAMPLES) goto failed;
  pcm->format = encoder->sample_fmt;
  pcm->sample_rate = RATE;
  pcm->nb_samples = FRAME_SAMPLES;
  if (av_channel_layout_copy(&pcm->ch_layout, &encoder->ch_layout) < 0 ||
      av_frame_get_buffer(pcm, 0) < 0) goto failed;
  return 1;

failed:
  ml_ac3_close();
  return 0;
}

static void encode_frame(void) {
  if (avcodec_send_frame(encoder, pcm) < 0 ||
      avcodec_receive_packet(encoder, packet) < 0 || packet->size != FRAME_BYTES) {
    __atomic_fetch_add(&esplus_stats.aout_drops, 1, __ATOMIC_RELAXED);
    av_packet_unref(packet);
    return;
  }

  pthread_mutex_lock(&queue_lock);
  if (queued == QUEUE_FRAMES) {
    read_at = (read_at + 1) % QUEUE_FRAMES;
    queued--;
    __atomic_fetch_add(&esplus_stats.aout_drops, 1, __ATOMIC_RELAXED);
  }
  memcpy(queue[(read_at + queued) % QUEUE_FRAMES], packet->data, FRAME_BYTES);
  queued++;
  pthread_mutex_unlock(&queue_lock);
  av_packet_unref(packet);
}

void ml_ac3_feed(const short *input, int frames) {
  while (frames > 0) {
    if (!samples && av_frame_make_writable(pcm) < 0) {
      __atomic_fetch_add(&esplus_stats.aout_drops, 1, __ATOMIC_RELAXED);
      return;
    }
    int count = FRAME_SAMPLES - samples;
    if (count > frames) count = frames;
    /* Moonlight PCM and FFmpeg's 5.1(side) use FL, FR, FC, LFE, SL, SR. */
    for (int c = 0; c < CHANNELS; c++) {
      int32_t *dst = (int32_t *)pcm->data[c] + samples;
      for (int i = 0; i < count; i++) dst[i] = (int32_t)input[i * CHANNELS + c] * 65536;
    }
    samples += count;
    input += count * CHANNELS;
    frames -= count;
    if (samples == FRAME_SAMPLES) {
      encode_frame();
      samples = 0;
    }
  }
}

void ml_ac3_submit(uint64_t video_ms, unsigned epoch) {
  if (__atomic_load_n(&mode, __ATOMIC_ACQUIRE) != ACTIVE) return;
  pthread_mutex_lock(&queue_lock);
  if (queued && (!timeline_valid || timeline_epoch != epoch || next_ms + 128 < video_ms)) {
    if (timeline_valid && timeline_epoch == epoch)
      __atomic_fetch_add(&esplus_stats.aout_underruns, 1, __ATOMIC_RELAXED);
    timeline_valid = 1;
    timeline_epoch = epoch;
    next_ms = video_ms;
  }
  while (queued && next_ms <= video_ms + 64) {
    esplusplayer_es_packet out = {
      ESPLUSPLAYER_STREAM_TYPE_AUDIO, (char *)queue[read_at], FRAME_BYTES,
      next_ms * 1000000ull, FRAME_MS * 1000000ull
    };
    int rc = esplus_submit_ep(&out, epoch);
    if (rc == ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED || rc == ESPLUSPLAYER_SUBMIT_STATUS_FULL) break;
    read_at = (read_at + 1) % QUEUE_FRAMES;
    queued--;
    next_ms += FRAME_MS;
    if (rc == ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS) {
      __atomic_fetch_add(&esplus_stats.aout_frames, 1, __ATOMIC_RELAXED);
    } else {
      __atomic_fetch_add(&esplus_stats.aout_drops, 1, __ATOMIC_RELAXED);
    }
  }
  __atomic_store_n(&esplus_stats.aout_buf_ms, queued * FRAME_MS, __ATOMIC_RELAXED);
  mldiag_anative(__atomic_load_n(&esplus_stats.aout_frames, __ATOMIC_RELAXED),
                 __atomic_load_n(&esplus_stats.aout_underruns, __ATOMIC_RELAXED), 0,
                 __atomic_load_n(&esplus_stats.aout_drops, __ATOMIC_RELAXED), queued * FRAME_MS);
  pthread_mutex_unlock(&queue_lock);
}

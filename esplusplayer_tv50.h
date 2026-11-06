/*
 * esplusplayer_tv50.h - C API for libesplusplayer.so (the "plusplayer" ES player
 * ABI) on Samsung TV Tizen 5.0.
 *
 * This is the older plusplayer ABI, which differs from the public esplusplayer
 * 0.0.3 (Tizen 6.5) headers in several ways:
 *   - Most functions return a C++ bool (1 = OK, 0 = fail), NOT a Tizen error
 *     code. A NULL handle or a not-implemented call also returns 0.
 *   - submit_packet returns esplusplayer_submit_status (4 = success).
 *   - es_packet.pts / duration are NANOSECONDS. The library divides them by
 *     1 000 000 internally (its internal unit is ms).
 *   - Callbacks have NO user_data argument.
 *   - esplusplayer_create() keeps ONE global player. A second create() deletes
 *     the first. Use one player per process.
 */
#ifndef ESPLUSPLAYER_TV50_H
#define ESPLUSPLAYER_TV50_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque player handle (a plusplayer C++ object). */
typedef void *esplusplayer_handle;

typedef enum {
  ESPLUSPLAYER_STREAM_TYPE_AUDIO = 0,
  ESPLUSPLAYER_STREAM_TYPE_VIDEO = 1,
} esplusplayer_stream_type;

/* Video codec selector. No AV1 / MJPEG on this version. */
typedef enum {
  ESPLUSPLAYER_VIDEO_MIME_TYPE_UNKNOWN = 0,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_H263 = 1,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_H264 = 2,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_HEVC = 3,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_MPEG1 = 4,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_MPEG2 = 5,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_MPEG4 = 6,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_VP8 = 7,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_VP9 = 8,
  ESPLUSPLAYER_VIDEO_MIME_TYPE_WMV3 = 9,
} esplusplayer_video_mime_type;

/* Only NONE/OVERLAY/EVAS are valid; other values abort the process. The
 * Ecore window path accepts only OVERLAY. */
typedef enum {
  ESPLUSPLAYER_DISPLAY_TYPE_NONE = 0,
  ESPLUSPLAYER_DISPLAY_TYPE_OVERLAY = 1,
  ESPLUSPLAYER_DISPLAY_TYPE_EVAS = 2,
} esplusplayer_display_type;

typedef enum {
  ESPLUSPLAYER_DISPLAY_MODE_LETTER_BOX = 0,
  ESPLUSPLAYER_DISPLAY_MODE_ORIGIN_SIZE,
  ESPLUSPLAYER_DISPLAY_MODE_FULL_SCREEN,
  ESPLUSPLAYER_DISPLAY_MODE_CROPPED_FULL,
  ESPLUSPLAYER_DISPLAY_MODE_ORIGIN_OR_LETTER,
  ESPLUSPLAYER_DISPLAY_MODE_DST_ROI,
} esplusplayer_display_mode;

typedef enum {
  ESPLUSPLAYER_BUFFER_STATUS_UNDERRUN = 0,
  ESPLUSPLAYER_BUFFER_STATUS_OVERRUN = 1,
} esplusplayer_buffer_status;

/* Returned by submit_packet: 0 not prepared (state <= NONE), 1 null packet or
 * no active track, 2 out of memory, 4 success. FULL (3) is defined for
 * completeness. */
typedef enum {
  ESPLUSPLAYER_SUBMIT_STATUS_NOT_PREPARED = 0,
  ESPLUSPLAYER_SUBMIT_STATUS_INVALID_PACKET = 1,
  ESPLUSPLAYER_SUBMIT_STATUS_OUT_OF_MEMORY = 2,
  ESPLUSPLAYER_SUBMIT_STATUS_FULL = 3,
  ESPLUSPLAYER_SUBMIT_STATUS_SUCCESS = 4,
} esplusplayer_submit_status;

/* 36 bytes. codec_data is copied (new[] + memcpy) when codec_data_length != 0. */
typedef struct {
  char *codec_data;           /* +0  */
  uint32_t codec_data_length; /* +4  */
  uint32_t mime_type;         /* +8  esplusplayer_video_mime_type */
  uint32_t width;             /* +12 */
  uint32_t height;            /* +16 */
  uint32_t max_width;         /* +20 */
  uint32_t max_height;        /* +24 */
  uint32_t framerate_num;     /* +28 */
  uint32_t framerate_den;     /* +32 */
} esplusplayer_video_stream_info;

/* Audio codec selector. NOTE: this ordering differs from public 0.0.3 - it
 * inserts DTS=7, so OPUS=8 (not 7). 9+ are PCM variants (audio/x-raw). */
typedef enum {
  ESPLUSPLAYER_AUDIO_MIME_TYPE_UNKNOWN = 0,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_AAC = 1,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_MP2 = 2,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_MP3 = 3,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_AC3 = 4,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_EAC3 = 5,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_VORBIS = 6,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_DTS = 7,
  ESPLUSPLAYER_AUDIO_MIME_TYPE_OPUS = 8,
  /* Assumed S16LE (the first PCM entry): must be validated on the TV. */
  ESPLUSPLAYER_AUDIO_MIME_TYPE_PCM_S16LE = 9,
} esplusplayer_audio_mime_type;

/* 24 bytes. */
typedef struct {
  char *codec_data;           /* +0  */
  uint32_t codec_data_length; /* +4  */
  uint32_t mime_type;         /* +8  esplusplayer_audio_mime_type */
  uint32_t bitrate;           /* +12 */
  uint32_t channels;          /* +16 */
  uint32_t sample_rate;       /* +20 */
} esplusplayer_audio_stream_info;

/* buffer is copied (new[] + memcpy); the caller keeps ownership.
 * buffer_size == 0 is handled as EOS for that track. */
typedef struct {
  uint32_t type;        /* +0  esplusplayer_stream_type */
  char *buffer;         /* +4  */
  uint32_t buffer_size; /* +8  */
  uint64_t pts;         /* +16 NANOSECONDS */
  uint64_t duration;    /* +24 NANOSECONDS */
} esplusplayer_es_packet;

#ifdef __cplusplus
static_assert(sizeof(esplusplayer_video_stream_info) == 36, "layout");
static_assert(sizeof(esplusplayer_audio_stream_info) == 24, "layout");
#else
_Static_assert(sizeof(esplusplayer_video_stream_info) == 36, "layout");
_Static_assert(sizeof(esplusplayer_audio_stream_info) == 24, "layout");
_Static_assert(__builtin_offsetof(esplusplayer_es_packet, pts) == 16, "layout");
_Static_assert(__builtin_offsetof(esplusplayer_es_packet, duration) == 24, "layout");
#endif

/* Callbacks take no user_data. */
typedef void (*esplusplayer_error_cb)(uint32_t plusplayer_error_type); /* raw value */
typedef void (*esplusplayer_buffer_status_cb)(uint32_t stream_type, uint32_t status);
typedef void (*esplusplayer_resource_conflicted_cb)(void); /* the player lost its resources to another app; only stop() leaves that state */
typedef void (*esplusplayer_eos_cb)(void);
typedef void (*esplusplayer_ready_to_prepare_cb)(uint32_t stream_type);
typedef void (*esplusplayer_prepare_async_done_cb)(int result_bool);

/* "int" returns below are a C++ bool (0/1) unless noted. */
esplusplayer_handle esplusplayer_create(void);
int esplusplayer_open(esplusplayer_handle h);
int esplusplayer_close(esplusplayer_handle h);
int esplusplayer_prepare_async(esplusplayer_handle h);
int esplusplayer_start(esplusplayer_handle h);
int esplusplayer_stop(esplusplayer_handle h);
int esplusplayer_pause(esplusplayer_handle h);
int esplusplayer_resume(esplusplayer_handle h);

/* Needs state > NONE (call after open) or returns 0. */
int esplusplayer_set_display(esplusplayer_handle h, esplusplayer_display_type type,
                             void *evas_object);
/* window = Ecore_Wl2_Window*; only DISPLAY_TYPE_OVERLAY. Runs on the ecore main
 * loop, so ecore must be initialised. */
int esplusplayer_set_ecore_display(esplusplayer_handle h, esplusplayer_display_type type,
                                   void *ecore_wl2_window, int x, int y, int w, int h_);
int esplusplayer_set_display_mode(esplusplayer_handle h, esplusplayer_display_mode mode);
int esplusplayer_set_display_visible(esplusplayer_handle h, int visible);

int esplusplayer_add_video_stream_info(esplusplayer_handle h,
                                       const esplusplayer_video_stream_info *info);
int esplusplayer_add_audio_stream_info(esplusplayer_handle h,
                                       const esplusplayer_audio_stream_info *info);

/* Returns esplusplayer_submit_status (NOT bool). */
int esplusplayer_submit_packet(esplusplayer_handle h, const esplusplayer_es_packet *pkt);
int esplusplayer_submit_eos_packet(esplusplayer_handle h, esplusplayer_stream_type type);

int esplusplayer_get_state(esplusplayer_handle h);
int esplusplayer_get_playing_time(esplusplayer_handle h, uint64_t *time);
int esplusplayer_set_audio_mute(esplusplayer_handle h, int mute);

void esplusplayer_set_error_cb(esplusplayer_handle h, esplusplayer_error_cb cb);
void esplusplayer_set_buffer_status_cb(esplusplayer_handle h, esplusplayer_buffer_status_cb cb);
void esplusplayer_set_resource_conflicted_cb(esplusplayer_handle h, esplusplayer_resource_conflicted_cb cb);
void esplusplayer_set_eos_cb(esplusplayer_handle h, esplusplayer_eos_cb cb);
void esplusplayer_set_ready_to_prepare_cb(esplusplayer_handle h, esplusplayer_ready_to_prepare_cb cb);
void esplusplayer_set_prepare_async_done_cb(esplusplayer_handle h, esplusplayer_prepare_async_done_cb cb);

/* Flushing seek: drops the data queued in the player pipelines (decoder and
 * sinks included) and continues playback from time_ms without stop/close.
 * time_ms is in milliseconds, the same unit esplusplayer_get_playing_time
 * reports. After the seek the segment restarts from the PTS of the first
 * submitted packet, so the caller should request a fresh reference frame
 * (IDR) right after. Returns a bool (1 = enqueued). Legal from the playing
 * state; after a resource conflict only stop() leaves the player. */
int esplusplayer_seek(esplusplayer_handle h, uint64_t time_ms);

/* Called when a seek issued with esplusplayer_seek() has completed. Like the
 * other callbacks in this ABI there is no user_data argument. */
typedef void (*esplusplayer_seek_done_cb)(void);
void esplusplayer_set_seek_done_cb(esplusplayer_handle h, esplusplayer_seek_done_cb cb);

#ifdef __cplusplus
}
#endif
#endif

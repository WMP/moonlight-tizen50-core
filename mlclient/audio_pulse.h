/*
 * audio_pulse.h - PCM output to the TV's PulseAudio (libpulse-simple, loaded at run time with dlopen:
 * no link-time dependency, no pulse headers). The esplusplayer has only a video stream (platform
 * limitation: the TV player rejects engine-fed audio streams, both Opus and raw PCM), so audio_opus.c
 * writes the decoded S16LE PCM here. All calls come from one thread at a time (moonlight-common-c's
 * audio thread, or init/cleanup, which it serialises).
 */
#ifndef AUDIO_PULSE_H
#define AUDIO_PULSE_H
#include <stddef.h>

/* Opens a low-latency S16LE playback stream. 0 = ok, -1 = no audio output (libpulse-simple missing or
   incomplete, or no server); the reason is recorded in mldiag. latency_ms 0 = the server default. */
int audio_pulse_open(int rate, int channels, int latency_ms);
/* Writes interleaved S16LE PCM; blocks while the server buffer is full (the back pressure). 0 = ok, -1 = error. */
int audio_pulse_write(const void *pcm, size_t bytes);
void audio_pulse_drain(void);   /* waits until the written audio has played */
void audio_pulse_close(void);   /* safe without a successful open; may be called twice */

#endif

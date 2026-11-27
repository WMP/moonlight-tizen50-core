#ifndef ML_AUDIO_AC3_H
#define ML_AUDIO_AC3_H

#include <stdint.h>

void ml_ac3_select(int enabled);
int ml_ac3_waiting(void);
void ml_ac3_ready(int active);
int ml_ac3_open(int rate, int channels);
void ml_ac3_close(void);
void ml_ac3_feed(const short *input, int frames);
void ml_ac3_submit(uint64_t video_ms, unsigned epoch);
int esplus_enable_ac3(void);

#endif

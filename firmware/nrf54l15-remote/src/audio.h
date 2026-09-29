/* 16 kHz mono capture (PDM mic or test tone) -> ADPCM -> ATVV notifications. */
#ifndef AUDIO_H_
#define AUDIO_H_

#include <stdint.h>

int audio_init(void);
/* Begin a new stream; AUDIO_START must already have been sent. */
void audio_start(uint8_t codec);
void audio_stop(void);

#endif /* AUDIO_H_ */

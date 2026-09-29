/* 16 kHz mono capture (PDM mic or test tone) -> ADPCM -> ATVV notifications. */
#ifndef AUDIO_H_
#define AUDIO_H_

#include <stdint.h>

int audio_init(void);
/* Begin a new stream of `frame_size`-byte notifications; AUDIO_START must
 * already have been sent. */
void audio_start(uint8_t codec, uint16_t frame_size);
void audio_stop(void);

#endif /* AUDIO_H_ */

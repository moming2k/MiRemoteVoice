/*
 * IMA/DVI ADPCM encoder matching mi-remote-bridge's ADPCMDecoder.swift.
 *
 * Portable C with no Zephyr dependencies so it can be unit-tested on the host
 * (see tests/host).
 */
#ifndef ADPCM_H_
#define ADPCM_H_

#include <stddef.h>
#include <stdint.h>

struct adpcm_state {
	int16_t predictor;
	uint8_t step_index;
};

void adpcm_reset(struct adpcm_state *state);

/* Encode one sample and return its 4-bit code. */
uint8_t adpcm_encode_sample(struct adpcm_state *state, int16_t sample);

/*
 * Encode `count` samples (must be even) into count / 2 bytes.
 *
 * Nibble order is HIGH nibble first: byte = (code[n] << 4) | code[n + 1].
 * This is what the Mac bridge decodes; many IMA libraries use the opposite
 * order, so do not swap in a third-party encoder without checking this.
 */
void adpcm_encode(struct adpcm_state *state, const int16_t *samples, size_t count,
		  uint8_t *out);

#endif /* ADPCM_H_ */

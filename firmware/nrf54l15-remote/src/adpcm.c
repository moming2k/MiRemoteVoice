/*
 * IMA/DVI ADPCM encoder. The quantisation and state update mirror
 * mi-remote-bridge/Sources/MiRemoteBridge/ATVV/ADPCMDecoder.swift exactly, so
 * the encoder's predictor always equals the decoder's output sample.
 */
#include "adpcm.h"

static const int8_t index_table[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static const int16_t step_table[89] = {
	7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
	25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
	88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
	307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
	1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2494,  2740,  3008,  3307,
	3638,  4002,  4402,  4842,  5327,  5860,  6446,  7091,  7800,  8580,  9438,  10382, 11420,
	12562, 13818, 15200, 16720, 18392, 20231, 22254, 24479, 26927, 29620, 32767,
};

void adpcm_reset(struct adpcm_state *state)
{
	state->predictor = 0;
	state->step_index = 0;
}

uint8_t adpcm_encode_sample(struct adpcm_state *state, int16_t sample)
{
	int step = step_table[state->step_index];
	int diff = (int)sample - (int)state->predictor;
	uint8_t code = 0;

	if (diff < 0) {
		code = 8;
		diff = -diff;
	}

	/* Same reconstruction as the decoder: step/8 + step + step/2 + step/4. */
	int delta = step >> 3;

	if (diff >= step) {
		code |= 4;
		diff -= step;
		delta += step;
	}
	if (diff >= (step >> 1)) {
		code |= 2;
		diff -= step >> 1;
		delta += step >> 1;
	}
	if (diff >= (step >> 2)) {
		code |= 1;
		delta += step >> 2;
	}

	int predictor = state->predictor + ((code & 8) ? -delta : delta);

	if (predictor > INT16_MAX) {
		predictor = INT16_MAX;
	} else if (predictor < INT16_MIN) {
		predictor = INT16_MIN;
	}
	state->predictor = (int16_t)predictor;

	int index = state->step_index + index_table[code & 7];

	if (index < 0) {
		index = 0;
	} else if (index > 88) {
		index = 88;
	}
	state->step_index = (uint8_t)index;

	return code;
}

void adpcm_encode(struct adpcm_state *state, const int16_t *samples, size_t count,
		  uint8_t *out)
{
	for (size_t i = 0; i + 1 < count; i += 2) {
		uint8_t hi = adpcm_encode_sample(state, samples[i]);
		uint8_t lo = adpcm_encode_sample(state, samples[i + 1]);

		out[i / 2] = (uint8_t)((hi << 4) | lo);
	}
}

#include "atvv_proto.h"

#include <string.h>

static void put_be16(uint8_t *buf, uint16_t value)
{
	buf[0] = (uint8_t)(value >> 8);
	buf[1] = (uint8_t)(value & 0xFF);
}

size_t atvv_build_caps_resp(uint8_t *buf, uint8_t codec, uint8_t model, uint16_t frame_size,
			    uint8_t extra_config)
{
	/* 0B | version 1.0 | codecs | interaction model | frame size | extra | rfu */
	buf[0] = ATVV_CTL_GET_CAPS_RESP;
	buf[1] = 0x01;
	buf[2] = 0x00;
	buf[3] = codec;
	buf[4] = model;
	put_be16(&buf[5], frame_size);
	buf[7] = extra_config;
	buf[8] = 0x00;
	return ATVV_CAPS_RESP_LEN;
}

size_t atvv_build_audio_start(uint8_t *buf, uint8_t reason, uint8_t codec, uint8_t stream_id)
{
	buf[0] = ATVV_CTL_AUDIO_START;
	buf[1] = reason;
	buf[2] = codec;
	buf[3] = stream_id;
	return ATVV_AUDIO_START_LEN;
}

size_t atvv_build_audio_sync(uint8_t *buf, uint8_t codec, uint16_t frame_num,
			     const struct adpcm_state *state)
{
	buf[0] = ATVV_CTL_AUDIO_SYNC;
	buf[1] = codec;
	put_be16(&buf[2], frame_num);
	put_be16(&buf[4], (uint16_t)state->predictor);
	buf[6] = state->step_index;
	return ATVV_AUDIO_SYNC_LEN;
}

size_t atvv_build_audio_stop(uint8_t *buf, uint8_t reason)
{
	buf[0] = ATVV_CTL_AUDIO_STOP;
	buf[1] = reason;
	return ATVV_AUDIO_STOP_LEN;
}

size_t atvv_build_start_search(uint8_t *buf)
{
	buf[0] = ATVV_CTL_START_SEARCH;
	return ATVV_START_SEARCH_LEN;
}

size_t atvv_build_mic_open_error(uint8_t *buf, uint16_t code)
{
	buf[0] = ATVV_CTL_MIC_OPEN_ERROR;
	put_be16(&buf[1], code);
	return ATVV_MIC_OPEN_ERR_LEN;
}

struct atvv_cmd atvv_parse_cmd(const uint8_t *data, size_t len)
{
	struct atvv_cmd cmd = {0};

	if (len == 0) {
		return cmd;
	}

	cmd.opcode = data[0];
	switch (cmd.opcode) {
	case ATVV_CMD_GET_CAPS:
		/* version(2) | legacy 0x0003(2) | supported interaction models(1).
		 * Hosts that omit the models field only support On-request. */
		cmd.host_models = len > 5 ? data[5] : ATVV_MODEL_ON_REQUEST;
		cmd.valid = true;
		break;
	case ATVV_CMD_MIC_OPEN:
		cmd.valid = true;
		break;
	case ATVV_CMD_MIC_CLOSE:
	case ATVV_CMD_MIC_EXTEND:
		/* v1.0 carries the stream id; v0.4 hosts send the bare opcode,
		 * which can only mean the current stream. */
		cmd.stream_id = len > 1 ? data[1] : ATVV_STREAM_ID_ANY;
		cmd.valid = true;
		break;
	default:
		break;
	}
	return cmd;
}

void atvv_framer_init(struct atvv_framer *f, const struct atvv_framer_ops *ops, void *ctx)
{
	memset(f, 0, sizeof(*f));
	f->ops = ops;
	f->ctx = ctx;
	f->frame_size = ATVV_FRAME_MAX;
}

void atvv_framer_start(struct atvv_framer *f, uint8_t codec, uint16_t frame_size)
{
	if (frame_size < 1) {
		frame_size = 1;
	} else if (frame_size > ATVV_FRAME_MAX) {
		frame_size = ATVV_FRAME_MAX;
	}

	adpcm_reset(&f->enc);
	f->frame_start = f->enc;
	f->codec = codec;
	f->frame_size = frame_size;
	f->fill = 0;
	f->frame_num = 0;
	f->resync_pending = true;
	f->have_odd = false;
	f->holding = false;
	f->count = 0;
	f->frames_sent = 0;
	f->frames_dropped = 0;
	f->backlog_dropped = 0;
}

/* Send one frame; `start`/`num` describe it for a possible AUDIO_SYNC. */
static void framer_send(struct atvv_framer *f, const uint8_t *data, size_t len,
			const struct adpcm_state *start, uint16_t num)
{
	bool send = true;

	if (f->resync_pending) {
		uint8_t sync[ATVV_AUDIO_SYNC_LEN];
		size_t sync_len = atvv_build_audio_sync(sync, f->codec, num, start);

		if (f->ops->send_ctl(f->ctx, sync, sync_len) == 0) {
			f->resync_pending = false;
		} else {
			/* The host would decode this frame with stale state: drop it
			 * and try the sync again before the next one. */
			send = false;
		}
	}

	if (send && f->ops->send_audio(f->ctx, data, len) == 0) {
		f->frames_sent++;
	} else {
		f->frames_dropped++;
		f->resync_pending = true;
	}
}

/* Backlog slot layout: predictor(2) step(1) pad(1) num(2) len(2) data. */
static uint8_t *slot_at(struct atvv_framer *f, size_t index)
{
	return f->backlog + ((f->head + index) % f->slots) * f->slot_size;
}

static void backlog_store(struct atvv_framer *f)
{
	if (f->count == f->slots) {
		/* Drop the oldest frame; the host must resync after the gap. */
		f->head = (f->head + 1) % f->slots;
		f->count--;
		f->backlog_dropped++;
		f->resync_pending = true;
	}

	uint8_t *slot = slot_at(f, f->count);
	uint16_t len = f->fill;

	memcpy(&slot[0], &f->frame_start.predictor, 2);
	slot[2] = f->frame_start.step_index;
	slot[3] = 0;
	memcpy(&slot[4], &f->frame_num, 2);
	memcpy(&slot[6], &len, 2);
	memcpy(&slot[8], f->buf, len);
	f->count++;
}

static void framer_emit(struct atvv_framer *f)
{
	if (f->holding) {
		backlog_store(f);
	} else {
		framer_send(f, f->buf, f->fill, &f->frame_start, f->frame_num);
	}
	f->frame_num++;
	f->fill = 0;
}

void atvv_framer_push(struct atvv_framer *f, const int16_t *samples, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		if (!f->have_odd) {
			f->odd_sample = samples[i];
			f->have_odd = true;
			continue;
		}
		f->have_odd = false;

		if (f->fill == 0) {
			f->frame_start = f->enc;
		}

		int16_t pair[2] = {f->odd_sample, samples[i]};

		adpcm_encode(&f->enc, pair, 2, &f->buf[f->fill]);
		f->fill++;

		if (f->fill >= f->frame_size) {
			framer_emit(f);
		}
	}
}

void atvv_framer_hold(struct atvv_framer *f, void *mem, size_t mem_size)
{
	f->backlog = mem;
	f->slot_size = ATVV_BACKLOG_SLOT_SIZE(f->frame_size);
	f->slots = mem_size / f->slot_size;
	f->head = 0;
	f->count = 0;
	f->backlog_dropped = 0;
	f->holding = f->slots > 0;
}

void atvv_framer_flush_partial(struct atvv_framer *f)
{
	f->have_odd = false;
	if (f->fill > 0) {
		framer_emit(f);
	}
}

void atvv_framer_release(struct atvv_framer *f)
{
	if (!f->holding) {
		return;
	}
	f->holding = false;

	for (size_t i = 0; i < f->count; i++) {
		const uint8_t *slot = slot_at(f, i);
		struct adpcm_state start;
		uint16_t num;
		uint16_t len;

		memcpy(&start.predictor, &slot[0], 2);
		start.step_index = slot[2];
		memcpy(&num, &slot[4], 2);
		memcpy(&len, &slot[6], 2);
		framer_send(f, &slot[8], len, &start, num);
	}
	f->count = 0;
}

size_t atvv_framer_backlog_bytes(const struct atvv_framer *f)
{
	size_t bytes = 0;

	for (size_t i = 0; i < f->count; i++) {
		const uint8_t *slot = f->backlog + ((f->head + i) % f->slots) * f->slot_size;
		uint16_t len;

		memcpy(&len, &slot[6], 2);
		bytes += len;
	}
	return bytes;
}

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
	f->frames_sent = 0;
	f->frames_dropped = 0;
}

static void framer_emit(struct atvv_framer *f)
{
	bool send = true;

	if (f->resync_pending) {
		uint8_t sync[ATVV_AUDIO_SYNC_LEN];
		size_t len = atvv_build_audio_sync(sync, f->codec, f->frame_num, &f->frame_start);

		if (f->ops->send_ctl(f->ctx, sync, len) == 0) {
			f->resync_pending = false;
		} else {
			/* The host would decode this frame with stale state: drop it
			 * and try the sync again before the next one. */
			send = false;
		}
	}

	if (send && f->ops->send_audio(f->ctx, f->buf, f->fill) == 0) {
		f->frames_sent++;
	} else {
		f->frames_dropped++;
		f->resync_pending = true;
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

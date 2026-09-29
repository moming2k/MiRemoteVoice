#include "remote_sm.h"

#include "atvv_proto.h"

void remote_sm_init(struct remote_sm *sm, const struct remote_sm_ops *ops, void *ctx,
		    uint16_t frame_size)
{
	sm->ops = ops;
	sm->ctx = ctx;
	sm->frame_size = frame_size;
	sm->button_held = false;
	sm->streaming = false;
	sm->stream_id = 0;
}

static void send_ctl(struct remote_sm *sm, const uint8_t *data, size_t len)
{
	sm->ops->send_ctl(sm->ctx, data, len);
}

static void stop_stream(struct remote_sm *sm, uint8_t reason, bool notify)
{
	if (!sm->streaming) {
		return;
	}
	sm->streaming = false;
	sm->ops->watchdog(sm->ctx, false);
	sm->ops->audio_stop(sm->ctx);

	if (notify) {
		uint8_t buf[ATVV_AUDIO_STOP_LEN];

		send_ctl(sm, buf, atvv_build_audio_stop(buf, reason));
	}
}

static void start_stream(struct remote_sm *sm)
{
	uint8_t buf[ATVV_AUDIO_START_LEN];

	/* Stream ids 1..127; 0 is reserved as "none". */
	sm->stream_id = (uint8_t)(sm->stream_id % 0x7F + 1);
	sm->streaming = true;

	send_ctl(sm, buf,
		 atvv_build_audio_start(buf, ATVV_START_REASON_HOLD_TO_TALK,
					ATVV_CODEC_ADPCM_16K, sm->stream_id));
	/* Start audio only after AUDIO_START so no frame can precede it. */
	sm->ops->audio_start(sm->ctx, ATVV_CODEC_ADPCM_16K);
	sm->ops->watchdog(sm->ctx, true);
}

void remote_sm_button(struct remote_sm *sm, bool pressed)
{
	if (pressed == sm->button_held) {
		return;
	}
	sm->button_held = pressed;

	if (pressed) {
		uint8_t buf[ATVV_START_SEARCH_LEN];

		sm->ops->hid_key(sm->ctx, true);
		send_ctl(sm, buf, atvv_build_start_search(buf));
	} else {
		stop_stream(sm, ATVV_STOP_REASON_RELEASED, true);
		sm->ops->hid_key(sm->ctx, false);
	}
}

void remote_sm_host_write(struct remote_sm *sm, const uint8_t *data, size_t len)
{
	struct atvv_cmd cmd = atvv_parse_cmd(data, len);
	uint8_t buf[ATVV_CTL_MAX_LEN];

	if (!cmd.valid) {
		return;
	}

	switch (cmd.opcode) {
	case ATVV_CMD_GET_CAPS:
		send_ctl(sm, buf,
			 atvv_build_caps_resp(buf, ATVV_CODEC_ADPCM_16K, ATVV_MODEL_HOLD_TO_TALK,
					      sm->frame_size));
		break;

	case ATVV_CMD_MIC_OPEN:
		if (sm->streaming) {
			break; /* duplicate open for the running stream */
		}
		if (!sm->button_held) {
			/* Short tap: the host answered START_SEARCH after release. */
			send_ctl(sm, buf, atvv_build_mic_open_error(buf, ATVV_MIC_OPEN_ERR_NOT_HELD));
			break;
		}
		start_stream(sm);
		break;

	case ATVV_CMD_MIC_CLOSE:
		if (cmd.stream_id == 0 || cmd.stream_id == sm->stream_id) {
			stop_stream(sm, ATVV_STOP_REASON_MIC_CLOSE, true);
		}
		break;

	case ATVV_CMD_MIC_EXTEND:
		if (sm->streaming && (cmd.stream_id == 0 || cmd.stream_id == sm->stream_id)) {
			sm->ops->watchdog(sm->ctx, true);
		}
		break;

	default:
		break;
	}
}

void remote_sm_timeout(struct remote_sm *sm)
{
	stop_stream(sm, ATVV_STOP_REASON_TIMEOUT, true);
}

void remote_sm_disconnected(struct remote_sm *sm)
{
	stop_stream(sm, ATVV_STOP_REASON_DISCONNECT, false);
	if (sm->button_held) {
		sm->button_held = false;
		sm->ops->hid_key(sm->ctx, false);
	}
}

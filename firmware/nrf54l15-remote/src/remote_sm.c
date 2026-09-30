#include "remote_sm.h"

#include "atvv_proto.h"

static void reset_link_state(struct remote_sm *sm)
{
	sm->frame_size = ATVV_DEFAULT_FRAME_SIZE;
	if (sm->frame_size > sm->max_frame_size) {
		sm->frame_size = sm->max_frame_size;
	}
	sm->model = ATVV_MODEL_ON_REQUEST;
	sm->caps_received = false;
	sm->audio_subscribed = false;
	sm->stream = REMOTE_STREAM_NONE;
	sm->stream_id = 0;
}

void remote_sm_init(struct remote_sm *sm, const struct remote_sm_ops *ops, void *ctx)
{
	sm->ops = ops;
	sm->ctx = ctx;
	sm->max_frame_size = ATVV_DEFAULT_FRAME_SIZE;
	sm->button_held = false;
	sm->remote_active = false;
	sm->last_htt_id = 0;
	reset_link_state(sm);
}

void remote_sm_set_max_frame_size(struct remote_sm *sm, uint16_t max_frame_size)
{
	sm->max_frame_size = max_frame_size ? max_frame_size : 1;
}

static void send_ctl(struct remote_sm *sm, const uint8_t *data, size_t len)
{
	sm->ops->send_ctl(sm->ctx, data, len);
}

static void send_mic_open_error(struct remote_sm *sm, uint16_t code)
{
	uint8_t buf[ATVV_MIC_OPEN_ERR_LEN];

	send_ctl(sm, buf, atvv_build_mic_open_error(buf, code));
}

static void stop_stream(struct remote_sm *sm, uint8_t reason, bool notify)
{
	if (sm->stream == REMOTE_STREAM_NONE) {
		return;
	}
	sm->stream = REMOTE_STREAM_NONE;
	sm->ops->transfer_timer(sm->ctx, false);
	sm->ops->audio_stop(sm->ctx);

	if (notify) {
		uint8_t buf[ATVV_AUDIO_STOP_LEN];

		send_ctl(sm, buf, atvv_build_audio_stop(buf, reason));
	}
}

static void start_stream(struct remote_sm *sm, enum remote_stream kind)
{
	uint8_t buf[ATVV_AUDIO_START_LEN];
	uint8_t reason;

	if (kind == REMOTE_STREAM_HTT) {
		/* Auto-incremented 0x01..0x80. */
		sm->last_htt_id = (uint8_t)(sm->last_htt_id % ATVV_STREAM_ID_HTT_MAX + 1);
		sm->stream_id = sm->last_htt_id;
		reason = ATVV_START_REASON_HOLD_TO_TALK;
	} else {
		sm->stream_id = ATVV_STREAM_ID_MIC_OPEN;
		reason = ATVV_START_REASON_MIC_OPEN;
	}
	sm->stream = kind;

	send_ctl(sm, buf,
		 atvv_build_audio_start(buf, reason, ATVV_CODEC_ADPCM_16K, sm->stream_id));
	/* Start audio only after AUDIO_START so no frame can precede it. */
	sm->ops->audio_start(sm->ctx, ATVV_CODEC_ADPCM_16K, sm->frame_size);
	sm->ops->transfer_timer(sm->ctx, true);
}

static bool stream_id_matches(const struct remote_sm *sm, uint8_t id)
{
	return sm->stream != REMOTE_STREAM_NONE &&
	       (id == ATVV_STREAM_ID_ANY || id == sm->stream_id);
}

void remote_sm_button(struct remote_sm *sm, bool pressed)
{
	if (pressed == sm->button_held) {
		return;
	}
	sm->button_held = pressed;

	if (!pressed) {
		if (sm->stream == REMOTE_STREAM_HTT) {
			stop_stream(sm, ATVV_STOP_REASON_HTT_RELEASED, true);
		}
		sm->ops->hid_key(sm->ctx, false);
		return;
	}

	sm->remote_active = true;
	sm->ops->user_activity(sm->ctx);

	if (sm->model == ATVV_MODEL_HOLD_TO_TALK) {
		sm->ops->hid_key(sm->ctx, true);
		/* Audio capture must not start without audio notifications. */
		if (sm->audio_subscribed) {
			stop_stream(sm, ATVV_STOP_REASON_UPCOMING_START, true);
			start_stream(sm, REMOTE_STREAM_HTT);
		}
	} else {
		uint8_t buf[ATVV_START_SEARCH_LEN];

		/* Spec: START_SEARCH first, then the HID event. */
		send_ctl(sm, buf, atvv_build_start_search(buf));
		sm->ops->hid_key(sm->ctx, true);
	}
}

static void handle_get_caps(struct remote_sm *sm, uint8_t host_models)
{
	uint8_t buf[ATVV_CAPS_RESP_LEN];

	/* Hold-to-Talk if the host supports it, otherwise On-request (we do
	 * not implement Press-to-Talk). */
	sm->model = (host_models & 0x02) ? ATVV_MODEL_HOLD_TO_TALK : ATVV_MODEL_ON_REQUEST;
	sm->frame_size = sm->max_frame_size;
	sm->caps_received = true;

	send_ctl(sm, buf,
		 atvv_build_caps_resp(buf, ATVV_CODEC_ADPCM_16K, sm->model, sm->frame_size,
				      ATVV_CAPS_EXTRA_DLE));
}

static void handle_mic_open(struct remote_sm *sm)
{
	if (!sm->audio_subscribed) {
		send_mic_open_error(sm, ATVV_MIC_OPEN_ERR_NOTIFY_DISABLED);
		return;
	}
	if (sm->stream == REMOTE_STREAM_HTT) {
		/* HTT/PTT streams cannot be interrupted by MIC_OPEN. */
		send_mic_open_error(sm, ATVV_MIC_OPEN_ERR_PTT_HTT_ACTIVE);
		return;
	}
	if (!sm->remote_active) {
		send_mic_open_error(sm, ATVV_MIC_OPEN_ERR_NOT_ACTIVE);
		return;
	}
	/* A repeated MIC_OPEN restarts the on-request stream. */
	stop_stream(sm, ATVV_STOP_REASON_UPCOMING_START, true);
	start_stream(sm, REMOTE_STREAM_ON_REQUEST);
}

void remote_sm_host_write(struct remote_sm *sm, const uint8_t *data, size_t len)
{
	struct atvv_cmd cmd = atvv_parse_cmd(data, len);

	if (!cmd.valid) {
		return;
	}

	switch (cmd.opcode) {
	case ATVV_CMD_GET_CAPS:
		handle_get_caps(sm, cmd.host_models);
		break;
	case ATVV_CMD_MIC_OPEN:
		handle_mic_open(sm);
		break;
	case ATVV_CMD_MIC_CLOSE:
		/* Mismatching ids are ignored (e.g. racing with a new stream). */
		if (stream_id_matches(sm, cmd.stream_id)) {
			stop_stream(sm, ATVV_STOP_REASON_MIC_CLOSE, true);
		}
		break;
	case ATVV_CMD_MIC_EXTEND:
		/* No reply, just restart the Audio Transfer Timeout. */
		if (stream_id_matches(sm, cmd.stream_id)) {
			sm->ops->transfer_timer(sm->ctx, true);
		}
		break;
	default:
		break;
	}
}

void remote_sm_audio_subscribed(struct remote_sm *sm, bool subscribed)
{
	sm->audio_subscribed = subscribed;
	if (!subscribed) {
		stop_stream(sm, ATVV_STOP_REASON_NOTIFY_DISABLED, true);
	}
}

void remote_sm_set_active(struct remote_sm *sm, bool active)
{
	sm->remote_active = active;
}

void remote_sm_timeout(struct remote_sm *sm, enum remote_timeout which)
{
	stop_stream(sm,
		    which == REMOTE_TIMEOUT_TRANSFER ? ATVV_STOP_REASON_TRANSFER_TIMEOUT
						     : ATVV_STOP_REASON_OTHER,
		    true);
}

void remote_sm_disconnected(struct remote_sm *sm)
{
	stop_stream(sm, ATVV_STOP_REASON_OTHER, false);
	if (sm->button_held) {
		sm->button_held = false;
		sm->ops->hid_key(sm->ctx, false);
	}
	/* Capabilities and subscriptions are renegotiated on every connection. */
	reset_link_state(sm);
}

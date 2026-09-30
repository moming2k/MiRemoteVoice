/*
 * Voice-button / ATVV state machine, independent of Zephyr so it can be
 * unit-tested on the host. Follows Google "Voice over BLE" spec v1.0.
 *
 * Interaction model is chosen from the host's GET_CAPS:
 *
 * Hold-to-Talk (host supports it; mi-remote-bridge always does):
 *   button down -> HID F5 down, AUDIO_START(reason 0x03, id 1..0x80), audio
 *   host MIC_EXTEND every few seconds resets the Audio Transfer Timeout
 *   button up   -> AUDIO_STOP(0x02), *then* HID F5 up (the bridge relies on
 *                  this order)
 *
 * On-request (default before GET_CAPS, and for hosts without HTT):
 *   button down -> START_SEARCH, then HID F5 down
 *   host MIC_OPEN -> AUDIO_START(reason 0x00, id 0x00), audio until the host
 *                    sends MIC_CLOSE or the Audio Transfer Timeout expires
 */
#ifndef REMOTE_SM_H_
#define REMOTE_SM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct remote_sm_ops {
	void (*send_ctl)(void *ctx, const uint8_t *data, size_t len);
	void (*hid_key)(void *ctx, bool pressed);
	void (*audio_start)(void *ctx, uint8_t codec, uint16_t frame_size);
	void (*audio_stop)(void *ctx);
	/* (Re)start (true) or cancel (false) the Audio Transfer Timeout. */
	void (*transfer_timer)(void *ctx, bool arm);
	/* The user touched the remote: restart the Active Remote Timeout. */
	void (*user_activity)(void *ctx);
};

enum remote_stream {
	REMOTE_STREAM_NONE,
	REMOTE_STREAM_ON_REQUEST,
	REMOTE_STREAM_HTT,
};

enum remote_timeout {
	REMOTE_TIMEOUT_TRANSFER, /* no MIC_EXTEND in time */
	REMOTE_TIMEOUT_MAX,      /* hard cap, e.g. a stuck button */
};

struct remote_sm {
	const struct remote_sm_ops *ops;
	void *ctx;
	uint16_t max_frame_size; /* upper bound from config / MTU */
	uint16_t frame_size;     /* negotiated in CAPS_RESP */
	uint8_t model;           /* ATVV_MODEL_* in use */
	bool caps_received;      /* GET_CAPS seen on this connection */
	bool button_held;
	bool audio_subscribed;
	bool remote_active; /* within the Active Remote Timeout */
	enum remote_stream stream;
	uint8_t stream_id;
	uint8_t last_htt_id;
};

void remote_sm_init(struct remote_sm *sm, const struct remote_sm_ops *ops, void *ctx);
/* Largest frame the link can carry (MTU - 3, capped by config). Takes effect
 * at the next GET_CAPS. */
void remote_sm_set_max_frame_size(struct remote_sm *sm, uint16_t max_frame_size);
void remote_sm_button(struct remote_sm *sm, bool pressed);
void remote_sm_host_write(struct remote_sm *sm, const uint8_t *data, size_t len);
void remote_sm_audio_subscribed(struct remote_sm *sm, bool subscribed);
/* Active Remote Timeout: while inactive, MIC_OPEN is refused (0x0F02).
 * A button press makes the remote active again. */
void remote_sm_set_active(struct remote_sm *sm, bool active);
void remote_sm_timeout(struct remote_sm *sm, enum remote_timeout which);
/* Link lost: stop audio and release the key locally without notifying. */
void remote_sm_disconnected(struct remote_sm *sm);

#endif /* REMOTE_SM_H_ */

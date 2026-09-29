/*
 * Voice-button state machine (hold-to-talk), independent of Zephyr so it can
 * be unit-tested on the host.
 *
 * Sequence expected by mi-remote-bridge:
 *   button down -> HID F5 down, START_SEARCH
 *   host MIC_OPEN -> AUDIO_START, then audio (framer sends AUDIO_SYNC first)
 *   host MIC_EXTEND every ~4 s while streaming
 *   button up -> AUDIO_STOP, *then* HID F5 up (the bridge relies on this order)
 */
#ifndef REMOTE_SM_H_
#define REMOTE_SM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct remote_sm_ops {
	void (*send_ctl)(void *ctx, const uint8_t *data, size_t len);
	void (*hid_key)(void *ctx, bool pressed);
	void (*audio_start)(void *ctx, uint8_t codec);
	void (*audio_stop)(void *ctx);
	/* Arm (true) or cancel (false) the host keep-alive watchdog. */
	void (*watchdog)(void *ctx, bool arm);
};

struct remote_sm {
	const struct remote_sm_ops *ops;
	void *ctx;
	uint16_t frame_size;
	bool button_held;
	bool streaming;
	uint8_t stream_id;
};

void remote_sm_init(struct remote_sm *sm, const struct remote_sm_ops *ops, void *ctx,
		    uint16_t frame_size);
void remote_sm_button(struct remote_sm *sm, bool pressed);
void remote_sm_host_write(struct remote_sm *sm, const uint8_t *data, size_t len);
/* Keep-alive watchdog or max stream duration expired. */
void remote_sm_timeout(struct remote_sm *sm);
/* Link lost: stop audio and release the key locally without notifying. */
void remote_sm_disconnected(struct remote_sm *sm);

#endif /* REMOTE_SM_H_ */

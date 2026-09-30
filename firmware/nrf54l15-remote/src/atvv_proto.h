/*
 * Android TV Voice over BLE (ATVV) v1.0 message encoding, as consumed by
 * mi-remote-bridge/Sources/MiRemoteBridge/ATVV/ATVVProtocol.swift.
 *
 * Portable C with no Zephyr dependencies so it can be unit-tested on the host.
 */
#ifndef ATVV_PROTO_H_
#define ATVV_PROTO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "adpcm.h"

/* 128-bit UUIDs: AB5E000x-5A21-4F05-BC7D-AF01F617B664 */
#define ATVV_UUID_SERVICE_VAL 0xAB5E0001
#define ATVV_UUID_TX_VAL      0xAB5E0002 /* host -> remote commands (write) */
#define ATVV_UUID_AUDIO_VAL   0xAB5E0003 /* remote -> host audio (notify) */
#define ATVV_UUID_CTL_VAL     0xAB5E0004 /* remote -> host control (notify) */

/* Host -> remote opcodes (written to the TX characteristic). */
#define ATVV_CMD_GET_CAPS   0x0A
#define ATVV_CMD_MIC_OPEN   0x0C
#define ATVV_CMD_MIC_CLOSE  0x0D
#define ATVV_CMD_MIC_EXTEND 0x0E

/* Remote -> host opcodes (notified on the CTL characteristic). */
#define ATVV_CTL_AUDIO_STOP     0x00
#define ATVV_CTL_AUDIO_START    0x04
#define ATVV_CTL_START_SEARCH   0x08
#define ATVV_CTL_AUDIO_SYNC     0x0A
#define ATVV_CTL_GET_CAPS_RESP  0x0B
#define ATVV_CTL_MIC_OPEN_ERROR 0x0C

#define ATVV_CODEC_ADPCM_8K  0x01
#define ATVV_CODEC_ADPCM_16K 0x02

/*
 * Values below are from Google "Voice over BLE" spec v1.0.
 */

/* Assistant interaction models (GET_CAPS: supported set; CAPS_RESP: chosen). */
#define ATVV_MODEL_ON_REQUEST   0x00
#define ATVV_MODEL_PRESS_TO_TALK 0x01
#define ATVV_MODEL_HOLD_TO_TALK 0x03

/* CAPS_RESP "extra configuration". */
#define ATVV_CAPS_EXTRA_DLE 0x01 /* ask the host to enable DLE / larger MTU */

/* AUDIO_START reason. */
#define ATVV_START_REASON_MIC_OPEN     0x00
#define ATVV_START_REASON_PRESS_TO_TALK 0x01
#define ATVV_START_REASON_HOLD_TO_TALK 0x03

/* AUDIO_STOP reason. */
#define ATVV_STOP_REASON_MIC_CLOSE       0x00
#define ATVV_STOP_REASON_HTT_RELEASED    0x02
#define ATVV_STOP_REASON_UPCOMING_START  0x04
#define ATVV_STOP_REASON_TRANSFER_TIMEOUT 0x08
#define ATVV_STOP_REASON_NOTIFY_DISABLED 0x10
#define ATVV_STOP_REASON_OTHER           0x80

/* MIC_OPEN_ERROR codes. */
#define ATVV_MIC_OPEN_ERR_NOT_ACTIVE      0x0F02 /* Active Remote Timeout expired */
#define ATVV_MIC_OPEN_ERR_NOTIFY_DISABLED 0x0F03 /* audio notifications off */
#define ATVV_MIC_OPEN_ERR_PTT_HTT_ACTIVE  0x0F80 /* PTT/HTT stream in progress */
#define ATVV_MIC_OPEN_ERR_INTERNAL        0x0FFF

/* Stream ids: 0x00 = opened by MIC_OPEN, 0x01..0x80 = PTT/HTT, 0xFF = any
 * (MIC_CLOSE / MIC_EXTEND only). */
#define ATVV_STREAM_ID_MIC_OPEN 0x00
#define ATVV_STREAM_ID_HTT_MIN  0x01
#define ATVV_STREAM_ID_HTT_MAX  0x80
#define ATVV_STREAM_ID_ANY      0xFF

/* Default audio frame size before CAPS negotiation. */
#define ATVV_DEFAULT_FRAME_SIZE 20

#define ATVV_CAPS_RESP_LEN    9
#define ATVV_AUDIO_START_LEN  4
#define ATVV_AUDIO_SYNC_LEN   7
#define ATVV_AUDIO_STOP_LEN   2
#define ATVV_START_SEARCH_LEN 1
#define ATVV_MIC_OPEN_ERR_LEN 3
#define ATVV_CTL_MAX_LEN      9

size_t atvv_build_caps_resp(uint8_t *buf, uint8_t codec, uint8_t model, uint16_t frame_size,
			    uint8_t extra_config);
size_t atvv_build_audio_start(uint8_t *buf, uint8_t reason, uint8_t codec, uint8_t stream_id);
size_t atvv_build_audio_sync(uint8_t *buf, uint8_t codec, uint16_t frame_num,
			     const struct adpcm_state *state);
size_t atvv_build_audio_stop(uint8_t *buf, uint8_t reason);
size_t atvv_build_start_search(uint8_t *buf);
size_t atvv_build_mic_open_error(uint8_t *buf, uint16_t code);

struct atvv_cmd {
	uint8_t opcode;      /* one of ATVV_CMD_*, or the raw byte if unknown */
	uint8_t stream_id;   /* MIC_CLOSE / MIC_EXTEND: stream id, ANY if absent */
	uint8_t host_models; /* GET_CAPS: supported interaction models */
	bool valid;
};

struct atvv_cmd atvv_parse_cmd(const uint8_t *data, size_t len);

/*
 * Framer: turns 16-bit PCM into fixed-size ADPCM notifications.
 *
 * When a notification cannot be sent, the host decoder loses sync with the
 * encoder. The framer then sends AUDIO_SYNC (carrying the encoder state at the
 * start of the next frame) before that frame, so the host can resynchronise.
 */
#define ATVV_FRAME_MAX 244 /* max ATT payload with a 247-byte MTU */

struct atvv_framer_ops {
	/* Return 0 on success, negative if the notification was not queued. */
	int (*send_audio)(void *ctx, const uint8_t *data, size_t len);
	int (*send_ctl)(void *ctx, const uint8_t *data, size_t len);
};

struct atvv_framer {
	const struct atvv_framer_ops *ops;
	void *ctx;
	struct adpcm_state enc;
	struct adpcm_state frame_start;
	uint8_t codec;
	uint16_t frame_size;
	uint16_t fill;
	uint16_t frame_num;
	bool resync_pending;
	bool have_odd;
	int16_t odd_sample;
	uint8_t buf[ATVV_FRAME_MAX];
	/* Backlog ring used while holding (see atvv_framer_hold). */
	bool holding;
	uint8_t *backlog;
	size_t slot_size;
	size_t slots;
	size_t head;  /* oldest stored frame */
	size_t count; /* stored frames */
	/* Statistics. */
	uint32_t frames_sent;
	uint32_t frames_dropped;
	uint32_t backlog_dropped;
};

void atvv_framer_init(struct atvv_framer *f, const struct atvv_framer_ops *ops, void *ctx);
/*
 * Reset encoder and counters for a new stream. frame_size (bytes per
 * notification) is clamped to [1, ATVV_FRAME_MAX]. The first frame of every
 * stream is preceded by an AUDIO_SYNC carrying the initial encoder state.
 */
void atvv_framer_start(struct atvv_framer *f, uint8_t codec, uint16_t frame_size);
void atvv_framer_push(struct atvv_framer *f, const int16_t *samples, size_t count);

/*
 * Holding: complete frames are kept in a ring in `mem` instead of being sent,
 * e.g. while the remote is still reconnecting. When the ring is full the
 * oldest frame is dropped. Call after atvv_framer_start().
 */
#define ATVV_BACKLOG_SLOT_SIZE(frame_size) (8 + (size_t)(frame_size))
void atvv_framer_hold(struct atvv_framer *f, void *mem, size_t mem_size);
/* Emit the current partial frame (a trailing odd sample is dropped). */
void atvv_framer_flush_partial(struct atvv_framer *f);
/* Send the backlog (preceded by AUDIO_SYNC) and continue live. */
void atvv_framer_release(struct atvv_framer *f);
size_t atvv_framer_backlog_bytes(const struct atvv_framer *f);

#endif /* ATVV_PROTO_H_ */

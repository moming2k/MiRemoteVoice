/*
 * Host unit tests for the portable firmware modules (adpcm, atvv_proto,
 * remote_sm). Run with `make -C tests/host`.
 *
 * The decoder and control-message parser below are line-by-line C ports of
 * mi-remote-bridge's ADPCMDecoder.swift and ATVVProtocol.swift, so these tests
 * check the firmware against what the Mac actually does.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adpcm.h"
#include "atvv_proto.h"
#include "battery_level.h"
#include "remote_sm.h"
#include "wake.h"

static int failures;

#define CHECK(cond, ...)                                                                           \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			failures++;                                                                \
			printf("FAIL %s:%d: ", __FILE__, __LINE__);                                \
			printf(__VA_ARGS__);                                                       \
			printf("\n");                                                              \
		}                                                                                  \
	} while (0)

/* ---- Port of ADPCMDecoder.swift ---- */

static const int dec_index_table[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const int dec_step_table[89] = {
	7,     8,     9,     10,    11,    12,    13,    14,    16,    17,    19,    21,    23,
	25,    28,    31,    34,    37,    41,    45,    50,    55,    60,    66,    73,    80,
	88,    97,    107,   118,   130,   143,   157,   173,   190,   209,   230,   253,   279,
	307,   337,   371,   408,   449,   494,   544,   598,   658,   724,   796,   876,   963,
	1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,  2272,  2494,  2740,  3008,  3307,
	3638,  4002,  4402,  4842,  5327,  5860,  6446,  7091,  7800,  8580,  9438,  10382, 11420,
	12562, 13818, 15200, 16720, 18392, 20231, 22254, 24479, 26927, 29620, 32767,
};

struct swift_decoder {
	int predictor;
	int step_index;
};

static void dec_reset(struct swift_decoder *d, int16_t predictor, uint8_t step_index)
{
	d->predictor = predictor;
	d->step_index = step_index > 88 ? 88 : step_index;
}

static int16_t dec_nibble(struct swift_decoder *d, int nibble)
{
	int step = dec_step_table[d->step_index];
	int difference = step >> 3;

	if (nibble & 4) {
		difference += step;
	}
	if (nibble & 2) {
		difference += step >> 1;
	}
	if (nibble & 1) {
		difference += step >> 2;
	}
	d->predictor += (nibble & 8) ? -difference : difference;
	if (d->predictor > 32767) {
		d->predictor = 32767;
	}
	if (d->predictor < -32768) {
		d->predictor = -32768;
	}
	d->step_index += dec_index_table[nibble & 7];
	if (d->step_index < 0) {
		d->step_index = 0;
	}
	if (d->step_index > 88) {
		d->step_index = 88;
	}
	return (int16_t)d->predictor;
}

/* High nibble first, exactly like decode(_:) in Swift. */
static size_t dec_bytes(struct swift_decoder *d, const uint8_t *bytes, size_t len, int16_t *out)
{
	size_t n = 0;

	for (size_t i = 0; i < len; i++) {
		out[n++] = dec_nibble(d, (bytes[i] >> 4) & 0x0f);
		out[n++] = dec_nibble(d, bytes[i] & 0x0f);
	}
	return n;
}

/* ---- Test signal ---- */

static void make_signal(int16_t *buf, size_t n)
{
	/* Chirp plus a step, to exercise small and large step indices. */
	for (size_t i = 0; i < n; i++) {
		double t = (double)i / 16000.0;
		double v = 9000.0 * sin(2 * M_PI * (200.0 + 1500.0 * t) * t);

		if (i > n / 2 && i < n / 2 + 400) {
			v += 15000.0;
		}
		buf[i] = (int16_t)v;
	}
}

/* ---- Tests ---- */

static void test_adpcm_matches_decoder(void)
{
	enum { N = 16000 };
	static int16_t in[N], out[N];
	static uint8_t enc[N / 2];
	struct adpcm_state st;
	struct swift_decoder dec;

	make_signal(in, N);
	adpcm_reset(&st);
	dec_reset(&dec, 0, 0);

	/* Encoder predictor must equal the decoder output sample by sample. */
	for (size_t i = 0; i < N; i += 2) {
		uint8_t hi = adpcm_encode_sample(&st, in[i]);
		int16_t p0 = st.predictor;
		uint8_t lo = adpcm_encode_sample(&st, in[i + 1]);
		int16_t p1 = st.predictor;

		enc[i / 2] = (uint8_t)((hi << 4) | lo);
		dec_bytes(&dec, &enc[i / 2], 1, &out[i]);
		CHECK(out[i] == p0 && out[i + 1] == p1, "predictor mismatch at %zu", i);
		if (out[i] != p0 || out[i + 1] != p1) {
			return;
		}
	}
	CHECK(dec.step_index == st.step_index, "step index diverged");

	double sig = 0, err = 0;

	for (size_t i = 0; i < N; i++) {
		sig += (double)in[i] * in[i];
		err += (double)(in[i] - out[i]) * (in[i] - out[i]);
	}
	double snr = 10 * log10(sig / err);

	CHECK(snr > 20.0, "ADPCM SNR too low: %.1f dB", snr);
	printf("  adpcm round trip SNR %.1f dB\n", snr);
}

static void test_adpcm_nibble_order(void)
{
	struct adpcm_state st;
	int16_t s[2] = {20000, 0};
	uint8_t byte;

	adpcm_reset(&st);
	adpcm_encode(&st, s, 2, &byte);
	/* First sample is a big positive jump (code 0x7), second swings back
	 * negative (sign bit set): high nibble must hold the first sample. */
	CHECK((byte >> 4) == 0x7, "first sample not in high nibble: 0x%02x", byte);
	CHECK((byte & 0x08) != 0, "second sample not in low nibble: 0x%02x", byte);
}

/* Port of ATVVProtocol.parseCapabilities for v1.0. */
static void test_caps_resp(void)
{
	uint8_t b[ATVV_CTL_MAX_LEN];
	size_t len = atvv_build_caps_resp(b, ATVV_CODEC_ADPCM_16K, ATVV_MODEL_HOLD_TO_TALK, 120,
					  ATVV_CAPS_EXTRA_DLE);

	CHECK(len >= 7, "caps too short for Swift parser");
	CHECK(b[0] == 0x0B, "opcode");
	CHECK(((b[1] << 8) | b[2]) == 0x0100, "version must be 1.0");
	/* The Swift parser swaps codec/model only if the codec byte has no
	 * known codec bit; make sure ours is taken as-is. */
	CHECK((b[3] & 0x03) != 0, "codec byte must carry a known codec bit");
	CHECK(b[3] & 0x02, "selectedCodec must be ADPCM 16 kHz");
	CHECK(((b[5] << 8) | b[6]) == 120, "frame size");
	CHECK(b[4] == 0x03 && b[7] == 0x01 && b[8] == 0x00, "model / extra / reserved");
}

static void test_ctl_layouts(void)
{
	uint8_t b[ATVV_CTL_MAX_LEN];
	struct adpcm_state st = {.predictor = -2000, .step_index = 30};

	/* ATVVProtocol.parseControl: 0x04 needs >= 4 bytes, codec at [2], id at [3]. */
	CHECK(atvv_build_audio_start(b, 3, ATVV_CODEC_ADPCM_16K, 9) == 4, "start len");
	CHECK(b[0] == 0x04 && b[2] == 0x02 && b[3] == 9, "start layout");

	/* 0x0A needs >= 7 bytes: codec, seq BE, predictor BE (signed), step. */
	CHECK(atvv_build_audio_sync(b, ATVV_CODEC_ADPCM_16K, 0x1234, &st) == 7, "sync len");
	CHECK(b[0] == 0x0A && b[1] == 0x02, "sync header");
	CHECK(((b[2] << 8) | b[3]) == 0x1234, "sync sequence");
	CHECK((int16_t)((b[4] << 8) | b[5]) == -2000, "sync predictor");
	CHECK(b[6] == 30, "sync step index");

	CHECK(atvv_build_audio_stop(b, 2) == 2 && b[0] == 0x00, "stop");
	CHECK(atvv_build_start_search(b) == 1 && b[0] == 0x08, "start search");
	CHECK(atvv_build_mic_open_error(b, 0x0F01) == 3 && b[0] == 0x0C && b[1] == 0x0F &&
		      b[2] == 0x01,
	      "mic open error");
}

static void test_parse_cmd(void)
{
	/* Exact bytes mi-remote-bridge writes. */
	const uint8_t get_caps[] = {0x0A, 0x01, 0x00, 0x00, 0x03, 0x03};
	const uint8_t mic_open[] = {0x0C, 0x00};
	const uint8_t mic_close[] = {0x0D, 0x05};
	const uint8_t extend[] = {0x0E, 0x05};
	struct atvv_cmd c;

	c = atvv_parse_cmd(get_caps, sizeof(get_caps));
	CHECK(c.valid && c.opcode == ATVV_CMD_GET_CAPS && c.host_models == 0x03, "get caps");
	c = atvv_parse_cmd(get_caps, 5); /* no models field: On-request only */
	CHECK(c.valid && c.host_models == ATVV_MODEL_ON_REQUEST, "get caps without models");
	c = atvv_parse_cmd(mic_open, sizeof(mic_open));
	CHECK(c.valid && c.opcode == ATVV_CMD_MIC_OPEN, "mic open");
	c = atvv_parse_cmd(mic_close, sizeof(mic_close));
	CHECK(c.valid && c.opcode == ATVV_CMD_MIC_CLOSE && c.stream_id == 5, "mic close");
	c = atvv_parse_cmd(extend, sizeof(extend));
	CHECK(c.valid && c.opcode == ATVV_CMD_MIC_EXTEND && c.stream_id == 5, "extend");
	c = atvv_parse_cmd((const uint8_t[]){0x0D}, 1); /* v0.4: bare opcode */
	CHECK(c.valid && c.stream_id == ATVV_STREAM_ID_ANY, "bare mic close means any stream");
	c = atvv_parse_cmd(NULL, 0);
	CHECK(!c.valid, "empty");
	c = atvv_parse_cmd((const uint8_t[]){0x42}, 1);
	CHECK(!c.valid, "unknown opcode");
}

/* ---- Framer: simulated host applying ATVVProtocol semantics ---- */

struct sim_host {
	struct swift_decoder dec;
	int16_t out[40000];
	size_t out_len;
	int fail_audio_at; /* drop the Nth audio notification (0-based), -1 none */
	int audio_calls;
	int syncs;
};

static int sim_send_audio(void *ctx, const uint8_t *data, size_t len)
{
	struct sim_host *h = ctx;

	if (h->audio_calls++ == h->fail_audio_at) {
		/* Simulate "no TX buffer": host never sees the frame, but the
		 * decoder output still needs a placeholder to keep indices. */
		memset(&h->out[h->out_len], 0, len * 2 * sizeof(int16_t));
		h->out_len += len * 2;
		return -12;
	}
	h->out_len += dec_bytes(&h->dec, data, len, &h->out[h->out_len]);
	return 0;
}

static int sim_send_ctl(void *ctx, const uint8_t *data, size_t len)
{
	struct sim_host *h = ctx;

	if (data[0] == ATVV_CTL_AUDIO_SYNC && len >= 7) {
		dec_reset(&h->dec, (int16_t)((data[4] << 8) | data[5]), data[6]);
		h->syncs++;
	}
	return 0;
}

static const struct atvv_framer_ops sim_ops = {sim_send_audio, sim_send_ctl};

static void run_framer(int fail_at, int *syncs, double *snr_after_drop, uint32_t *dropped)
{
	enum { N = 16000, FRAME = 120 };
	static int16_t in[N];
	static struct sim_host host;
	struct atvv_framer f;

	memset(&host, 0, sizeof(host));
	host.fail_audio_at = fail_at;
	dec_reset(&host.dec, 1234, 50); /* stale state from an earlier stream */
	make_signal(in, N);

	atvv_framer_init(&f, &sim_ops, &host);
	atvv_framer_start(&f, ATVV_CODEC_ADPCM_16K, FRAME);
	/* Push in odd-sized chunks to exercise sample pairing. */
	for (size_t i = 0; i < N;) {
		size_t chunk = (i / 7) % 2 ? 161 : 159;

		if (i + chunk > N) {
			chunk = N - i;
		}
		atvv_framer_push(&f, &in[i], chunk);
		i += chunk;
	}

	size_t start = fail_at >= 0 ? (size_t)(fail_at + 1) * FRAME * 2 : 0;
	double sig = 0, err = 0;

	for (size_t i = start; i < host.out_len; i++) {
		sig += (double)in[i] * in[i];
		err += (double)(in[i] - host.out[i]) * (in[i] - host.out[i]);
	}
	*syncs = host.syncs;
	*snr_after_drop = 10 * log10(sig / (err + 1e-9));
	*dropped = f.frames_dropped;

	CHECK(host.out_len == (N / (FRAME * 2)) * FRAME * 2, "unexpected sample count %zu",
	      host.out_len);
}

static void test_framer(void)
{
	int syncs;
	double snr;
	uint32_t dropped;

	run_framer(-1, &syncs, &snr, &dropped);
	CHECK(syncs == 1, "expected one initial AUDIO_SYNC, got %d", syncs);
	CHECK(dropped == 0, "unexpected drops");
	CHECK(snr > 20.0, "framer SNR %.1f dB", snr);
	printf("  framer SNR %.1f dB\n", snr);

	/* A dropped frame must be followed by a resync so later audio decodes. */
	run_framer(10, &syncs, &snr, &dropped);
	CHECK(dropped == 1, "expected one drop, got %u", dropped);
	CHECK(syncs == 2, "expected resync after drop, got %d syncs", syncs);
	CHECK(snr > 20.0, "post-drop SNR %.1f dB (resync failed?)", snr);
	printf("  framer SNR after dropped frame %.1f dB\n", snr);
}

/* ---- State machine ---- */

struct sm_log {
	char events[64][16];
	int n;
	bool audio_on;
	uint16_t frame_size;
	bool timer_on;
	int activity;
};

static void log_ev(struct sm_log *l, const char *s)
{
	if (l->n < 64) {
		snprintf(l->events[l->n++], 16, "%s", s);
	}
}

static void t_send_ctl(void *ctx, const uint8_t *d, size_t len)
{
	char s[16];

	/* Opcode plus the reason byte, or the full MIC_OPEN_ERROR code. */
	if (d[0] == ATVV_CTL_MIC_OPEN_ERROR && len == 3) {
		snprintf(s, sizeof(s), "0C:%02X%02X", d[1], d[2]);
	} else if (len > 1) {
		snprintf(s, sizeof(s), "%02X:%02X", d[0], d[1]);
	} else {
		snprintf(s, sizeof(s), "%02X", d[0]);
	}
	log_ev(ctx, s);
}

static void t_hid(void *ctx, bool p)
{
	log_ev(ctx, p ? "keydown" : "keyup");
}

static void t_audio_start(void *ctx, uint8_t codec, uint16_t frame_size)
{
	struct sm_log *l = ctx;

	l->audio_on = true;
	l->frame_size = frame_size;
	log_ev(ctx, "audio+");
}

static void t_audio_stop(void *ctx)
{
	((struct sm_log *)ctx)->audio_on = false;
	log_ev(ctx, "audio-");
}

static void t_timer(void *ctx, bool arm)
{
	((struct sm_log *)ctx)->timer_on = arm;
}

static void t_activity(void *ctx)
{
	((struct sm_log *)ctx)->activity++;
}

static const struct remote_sm_ops t_ops = {t_send_ctl, t_hid,   t_audio_start,
					   t_audio_stop, t_timer, t_activity};

static bool expect_seq(struct sm_log *l, const char *const *seq, int n)
{
	bool ok = l->n == n;

	for (int i = 0; ok && i < n; i++) {
		ok = strcmp(l->events[i], seq[i]) == 0;
	}
	if (!ok) {
		printf("    got:");
		for (int i = 0; i < l->n; i++) {
			printf(" %s", l->events[i]);
		}
		printf("\n");
	}
	return ok;
}

#define EXPECT(log, ...)                                                                           \
	do {                                                                                       \
		const char *const want_[] = {__VA_ARGS__};                                         \
		CHECK(expect_seq((log), want_, (int)(sizeof(want_) / sizeof(want_[0]))),          \
		      "unexpected event sequence");                                                \
		(log)->n = 0;                                                                      \
	} while (0)

static const uint8_t bridge_get_caps[] = {0x0A, 0x01, 0x00, 0x00, 0x03, 0x03};
static const uint8_t on_request_get_caps[] = {0x0A, 0x01, 0x00, 0x00, 0x03, 0x00};
static const uint8_t mic_open[] = {0x0C, 0x00};

static void sm_setup(struct remote_sm *sm, struct sm_log *log, const uint8_t *caps, size_t len)
{
	memset(log, 0, sizeof(*log));
	remote_sm_init(sm, &t_ops, log);
	remote_sm_set_max_frame_size(sm, 120);
	remote_sm_audio_subscribed(sm, true);
	if (caps) {
		remote_sm_host_write(sm, caps, len);
	}
}

/* What mi-remote-bridge does: GET_CAPS with HTT support, then hold. */
static void test_sm_bridge_hold_to_talk(void)
{
	struct remote_sm sm;
	struct sm_log log;

	sm_setup(&sm, &log, bridge_get_caps, sizeof(bridge_get_caps));
	EXPECT(&log, "0B:01");
	CHECK(sm.model == ATVV_MODEL_HOLD_TO_TALK, "HTT not selected");

	remote_sm_button(&sm, true);
	EXPECT(&log, "keydown", "04:03", "audio+");
	CHECK(log.frame_size == 120, "negotiated frame size not used: %u", log.frame_size);
	CHECK(sm.stream_id == 1 && log.timer_on, "stream id %u / timer", sm.stream_id);

	/* MIC_EXTEND for this stream: no reply. The bridge's START_SEARCH
	 * handler is never triggered because no START_SEARCH is sent. */
	remote_sm_host_write(&sm, (const uint8_t[]){0x0E, 0x01}, 2);
	CHECK(log.n == 0 && log.timer_on, "MIC_EXTEND must not reply");

	/* MIC_OPEN during HTT: error 0x0F80, stream keeps running. */
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	EXPECT(&log, "0C:0F80");
	CHECK(log.audio_on, "MIC_OPEN interrupted HTT stream");

	/* Release: AUDIO_STOP(0x02) strictly before HID key-up. */
	remote_sm_button(&sm, false);
	EXPECT(&log, "audio-", "00:02", "keyup");
	CHECK(!log.timer_on, "timer left running");

	/* Stream ids auto-increment. */
	remote_sm_button(&sm, true);
	CHECK(sm.stream_id == 2, "stream id did not increment");
	remote_sm_button(&sm, false);
}

static void test_sm_htt_edge_cases(void)
{
	struct remote_sm sm;
	struct sm_log log;

	/* Without audio notifications HTT only sends the key. */
	sm_setup(&sm, &log, bridge_get_caps, sizeof(bridge_get_caps));
	log.n = 0;
	remote_sm_audio_subscribed(&sm, false);
	remote_sm_button(&sm, true);
	EXPECT(&log, "keydown");
	remote_sm_button(&sm, false);
	EXPECT(&log, "keyup");

	/* Notifications disabled mid-stream -> AUDIO_STOP(0x10). */
	remote_sm_audio_subscribed(&sm, true);
	remote_sm_button(&sm, true);
	log.n = 0;
	remote_sm_audio_subscribed(&sm, false);
	EXPECT(&log, "audio-", "00:10");
	remote_sm_button(&sm, false);
	EXPECT(&log, "keyup");

	/* Transfer timeout -> 0x08, stuck button cap -> 0x80. */
	remote_sm_audio_subscribed(&sm, true);
	remote_sm_button(&sm, true);
	log.n = 0;
	remote_sm_timeout(&sm, REMOTE_TIMEOUT_TRANSFER);
	EXPECT(&log, "audio-", "00:08");
	remote_sm_button(&sm, false);
	remote_sm_button(&sm, true);
	log.n = 0;
	remote_sm_timeout(&sm, REMOTE_TIMEOUT_MAX);
	EXPECT(&log, "audio-", "00:80");
	/* Release after the stream already ended: key-up only. */
	remote_sm_button(&sm, false);
	EXPECT(&log, "keyup");

	/* MIC_CLOSE: wrong id ignored, 0xFF closes anything. */
	remote_sm_button(&sm, true);
	log.n = 0;
	remote_sm_host_write(&sm, (const uint8_t[]){0x0D, (uint8_t)(sm.stream_id + 1)}, 2);
	CHECK(log.n == 0 && log.audio_on, "wrong-id MIC_CLOSE acted");
	remote_sm_host_write(&sm, (const uint8_t[]){0x0D, 0xFF}, 2);
	EXPECT(&log, "audio-", "00:00");
	remote_sm_button(&sm, false);

	/* HTT ids stay within 0x01..0x80 and wrap. */
	for (int i = 0; i < 300; i++) {
		remote_sm_button(&sm, true);
		CHECK(sm.stream_id >= 0x01 && sm.stream_id <= 0x80, "id %u", sm.stream_id);
		remote_sm_button(&sm, false);
	}
}

static void test_sm_on_request(void)
{
	struct remote_sm sm;
	struct sm_log log;

	sm_setup(&sm, &log, on_request_get_caps, sizeof(on_request_get_caps));
	EXPECT(&log, "0B:01");
	CHECK(sm.model == ATVV_MODEL_ON_REQUEST, "On-request not selected");

	/* START_SEARCH precedes the HID key event. */
	remote_sm_button(&sm, true);
	EXPECT(&log, "08", "keydown");
	remote_sm_button(&sm, false);
	EXPECT(&log, "keyup");

	/* MIC_OPEN within the Active Remote Timeout: stream id 0, reason 0. */
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	EXPECT(&log, "04:00", "audio+");
	CHECK(sm.stream_id == 0, "on-request stream id must be 0");

	/* Repeated MIC_OPEN restarts the stream. */
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	EXPECT(&log, "audio-", "00:04", "04:00", "audio+");

	/* MIC_CLOSE(0x00) closes it. */
	remote_sm_host_write(&sm, (const uint8_t[]){0x0D, 0x00}, 2);
	EXPECT(&log, "audio-", "00:00");

	/* After the Active Remote Timeout, MIC_OPEN is refused (0x0F02). */
	remote_sm_set_active(&sm, false);
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	EXPECT(&log, "0C:0F02");
	CHECK(!log.audio_on, "inactive remote opened the mic");

	/* Without notifications: 0x0F03. */
	remote_sm_button(&sm, true);
	remote_sm_button(&sm, false);
	remote_sm_audio_subscribed(&sm, false);
	log.n = 0;
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	EXPECT(&log, "0C:0F03");

	/* HTT press during an on-request stream is not possible in this
	 * model, but a host that later negotiates HTT interrupts it: 0x04. */
	remote_sm_audio_subscribed(&sm, true);
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	remote_sm_host_write(&sm, bridge_get_caps, sizeof(bridge_get_caps));
	log.n = 0;
	remote_sm_button(&sm, true);
	EXPECT(&log, "keydown", "audio-", "00:04", "04:03", "audio+");
	remote_sm_button(&sm, false);
}

static void test_sm_default_and_disconnect(void)
{
	struct remote_sm sm;
	struct sm_log log;

	/* Before any GET_CAPS the remote is On-request with 20-byte frames. */
	sm_setup(&sm, &log, NULL, 0);
	CHECK(sm.model == ATVV_MODEL_ON_REQUEST && sm.frame_size == 20, "defaults");
	remote_sm_button(&sm, true);
	EXPECT(&log, "08", "keydown");
	CHECK(log.activity == 1, "user activity not reported");

	/* Disconnect: silent stop, key released, link state reset. */
	remote_sm_host_write(&sm, bridge_get_caps, sizeof(bridge_get_caps));
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	log.n = 0;
	remote_sm_disconnected(&sm);
	EXPECT(&log, "audio-", "keyup");
	CHECK(sm.model == ATVV_MODEL_ON_REQUEST && !sm.audio_subscribed &&
		      sm.stream == REMOTE_STREAM_NONE,
	      "link state not reset");
}

static void test_battery_level(void)
{
	const struct battery_point *c = battery_curve_lipo;
	size_t n = battery_curve_lipo_len;
	const struct battery_point lin[] = {{3000, 100}, {2000, 0}};

	CHECK(battery_level_pct(4300, c, n) == 100, "above full");
	CHECK(battery_level_pct(4200, c, n) == 100, "full");
	CHECK(battery_level_pct(3000, c, n) == 0, "below empty");
	CHECK(battery_level_pct(3800, c, n) == 52, "table point");
	CHECK(battery_level_pct(3850, c, n) == 58, "interpolated: %u", battery_level_pct(3850, c, n));
	CHECK(battery_level_pct(2500, lin, 2) == 50, "linear midpoint");

	/* Monotonic over the whole range. */
	uint8_t prev = 0;

	for (uint16_t mv = 3000; mv <= 4400; mv += 5) {
		uint8_t p = battery_level_pct(mv, c, n);

		CHECK(p >= prev, "not monotonic at %u mV", mv);
		prev = p;
	}
}


/* Holding: frames go to the backlog; release sends them with a leading
 * AUDIO_SYNC, then live audio continues seamlessly. */
static void run_backlog(size_t ring_frames, size_t hold_samples, size_t live_samples,
			uint32_t *dropped, double *snr, size_t *delivered_from)
{
	enum { FRAME = 120, N = 16000 * 3 };
	static int16_t in[N];
	static struct sim_host host;
	static uint8_t ring[64 * ATVV_BACKLOG_SLOT_SIZE(120)];
	struct atvv_framer f;

	memset(&host, 0, sizeof(host));
	host.fail_audio_at = -1;
	dec_reset(&host.dec, -777, 60); /* stale host state */
	make_signal(in, N);

	atvv_framer_init(&f, &sim_ops, &host);
	atvv_framer_start(&f, ATVV_CODEC_ADPCM_16K, FRAME);
	atvv_framer_hold(&f, ring, ring_frames * ATVV_BACKLOG_SLOT_SIZE(FRAME));
	CHECK(f.slots == ring_frames, "slots %zu", f.slots);

	atvv_framer_push(&f, in, hold_samples);
	CHECK(host.out_len == 0 && host.syncs == 0, "sent while holding");
	size_t expect_bytes = (hold_samples / (FRAME * 2) < ring_frames
				       ? hold_samples / (FRAME * 2)
				       : ring_frames) *
			      FRAME;

	CHECK(atvv_framer_backlog_bytes(&f) == expect_bytes, "backlog bytes %zu, want %zu",
	      atvv_framer_backlog_bytes(&f), expect_bytes);

	atvv_framer_release(&f);
	atvv_framer_push(&f, &in[hold_samples], live_samples);

	*dropped = f.backlog_dropped;
	*delivered_from = (size_t)f.backlog_dropped * FRAME * 2;
	CHECK(host.syncs == 1, "expected exactly one AUDIO_SYNC, got %d", host.syncs);

	/* Exact check: a continuous reference encoder's reconstruction is what
	 * a correctly synchronised decoder must output, sample for sample. */
	static int16_t recon[N];
	struct adpcm_state ref;
	size_t mismatches = 0;

	adpcm_reset(&ref);
	for (size_t i = 0; i < hold_samples + live_samples; i++) {
		adpcm_encode_sample(&ref, in[i]);
		recon[i] = ref.predictor;
	}
	for (size_t i = 0; i < host.out_len; i++) {
		mismatches += host.out[i] != recon[*delivered_from + i];
	}
	CHECK(mismatches == 0, "%zu decoded samples differ from the encoder", mismatches);

	double sig = 0, err = 0;

	for (size_t i = 0; i < host.out_len; i++) {
		int16_t r = in[*delivered_from + i];

		sig += (double)r * r;
		err += (double)(r - host.out[i]) * (r - host.out[i]);
	}
	*snr = 10 * log10(sig / (err + 1e-9));
}

static void test_framer_backlog(void)
{
	uint32_t dropped;
	double snr;
	size_t from;

	/* Fits: 20 frames held in a 40-frame ring, then 1 s live. */
	run_backlog(40, 20 * 240, 16000, &dropped, &snr, &from);
	CHECK(dropped == 0, "unexpected backlog drops %u", dropped);
	CHECK(snr > 15.0, "backlog SNR %.1f dB", snr);
	printf("  backlog (no overflow) SNR %.1f dB\n", snr);

	/* Overflow: 50 frames into a 16-frame ring keeps the newest 16. */
	run_backlog(16, 50 * 240, 16000, &dropped, &snr, &from);
	CHECK(dropped == 34, "expected 34 dropped frames, got %u", dropped);
	CHECK(snr > 15.0, "backlog-after-overflow SNR %.1f dB", snr);
	printf("  backlog (overflow) SNR %.1f dB\n", snr);
}

static void test_framer_flush_partial(void)
{
	static struct sim_host host;
	static uint8_t ring[8 * ATVV_BACKLOG_SLOT_SIZE(120)];
	struct atvv_framer f;
	int16_t in[101];

	memset(&host, 0, sizeof(host));
	host.fail_audio_at = -1;
	for (int i = 0; i < 101; i++) {
		in[i] = (int16_t)(i * 50);
	}
	atvv_framer_init(&f, &sim_ops, &host);
	atvv_framer_start(&f, ATVV_CODEC_ADPCM_16K, 120);
	atvv_framer_hold(&f, ring, sizeof(ring));
	atvv_framer_push(&f, in, 101);
	CHECK(atvv_framer_backlog_bytes(&f) == 0, "partial frame stored early");
	atvv_framer_flush_partial(&f);
	CHECK(atvv_framer_backlog_bytes(&f) == 50, "partial frame bytes %zu",
	      atvv_framer_backlog_bytes(&f));
	atvv_framer_release(&f);
	CHECK(host.out_len == 100, "decoded %zu samples", host.out_len);
}

static void test_wake(void)
{
	struct wake w;
	enum wake_action a;

	/* Not pressed at boot: nothing to do, events pass through. */
	wake_init(&w, false, 500);
	CHECK(!wake_pending(&w), "pending without press");
	CHECK(!wake_button(&w, true, &a) && a == WAKE_NONE, "idle consumed a press");

	/* Held until ready with HTT: deliver as a live hold. */
	wake_init(&w, true, 500);
	CHECK(wake_button(&w, true, &a) && a == WAKE_NONE, "repeat press");
	CHECK(wake_ready(&w, true, 1200) == WAKE_DELIVER_HOLD, "held -> hold");
	CHECK(!wake_pending(&w), "still pending after ready");
	CHECK(!wake_button(&w, false, &a), "release after delivery must reach the SM");

	/* Spoke and released before ready: replay. */
	wake_init(&w, true, 500);
	CHECK(wake_button(&w, false, &a) && a == WAKE_FREEZE, "release -> freeze");
	CHECK(wake_ready(&w, true, 900) == WAKE_DELIVER_REPLAY, "speech -> replay");

	/* Just a tap to wake: discard. */
	wake_init(&w, true, 500);
	wake_button(&w, false, &a);
	CHECK(wake_ready(&w, true, 200) == WAKE_DISCARD, "tap -> discard");

	/* Tap to wake, then hold while reconnecting: restart and deliver hold. */
	wake_init(&w, true, 500);
	wake_button(&w, false, &a);
	CHECK(wake_button(&w, true, &a) && a == WAKE_RESTART, "second press -> restart");
	CHECK(wake_ready(&w, true, 100) == WAKE_DELIVER_HOLD, "restarted hold");

	/* Host without Hold-to-Talk. */
	wake_init(&w, true, 500);
	CHECK(wake_ready(&w, false, 3000) == WAKE_DELIVER_PRESS, "on-request held");
	wake_init(&w, true, 500);
	wake_button(&w, false, &a);
	CHECK(wake_ready(&w, false, 3000) == WAKE_DISCARD, "on-request released");

	/* Timeout. */
	wake_init(&w, true, 500);
	CHECK(wake_timeout(&w) == WAKE_DISCARD && !wake_pending(&w), "timeout");
	CHECK(wake_timeout(&w) == WAKE_NONE, "second timeout");
	CHECK(wake_ready(&w, true, 3000) == WAKE_NONE, "ready after timeout");
}

static void test_sm_caps_flag(void)
{
	struct remote_sm sm;
	struct sm_log log;

	sm_setup(&sm, &log, NULL, 0);
	CHECK(!sm.caps_received, "caps flag set before GET_CAPS");
	remote_sm_host_write(&sm, bridge_get_caps, sizeof(bridge_get_caps));
	CHECK(sm.caps_received, "caps flag not set");
	remote_sm_disconnected(&sm);
	CHECK(!sm.caps_received, "caps flag survived disconnect");
}

int main(void)
{
	printf("adpcm matches Swift decoder\n");
	test_adpcm_matches_decoder();
	printf("adpcm nibble order\n");
	test_adpcm_nibble_order();
	printf("ATVV message layouts\n");
	test_caps_resp();
	test_ctl_layouts();
	test_parse_cmd();
	printf("framer\n");
	test_framer();
	printf("framer backlog\n");
	test_framer_backlog();
	test_framer_flush_partial();
	printf("wake press\n");
	test_wake();
	test_sm_caps_flag();
	printf("battery level\n");
	test_battery_level();
	printf("state machine\n");
	test_sm_bridge_hold_to_talk();
	test_sm_htt_edge_cases();
	test_sm_on_request();
	test_sm_default_and_disconnect();

	if (failures) {
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}

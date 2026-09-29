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
#include "remote_sm.h"

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
	size_t len = atvv_build_caps_resp(b, ATVV_CODEC_ADPCM_16K, ATVV_MODEL_HOLD_TO_TALK, 120);

	CHECK(len >= 7, "caps too short for Swift parser");
	CHECK(b[0] == 0x0B, "opcode");
	CHECK(((b[1] << 8) | b[2]) == 0x0100, "version must be 1.0");
	/* The Swift parser swaps codec/model only if the codec byte has no
	 * known codec bit; make sure ours is taken as-is. */
	CHECK((b[3] & 0x03) != 0, "codec byte must carry a known codec bit");
	CHECK(b[3] & 0x02, "selectedCodec must be ADPCM 16 kHz");
	CHECK(((b[5] << 8) | b[6]) == 120, "frame size");
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
	CHECK(c.valid && c.opcode == ATVV_CMD_GET_CAPS, "get caps");
	c = atvv_parse_cmd(mic_open, sizeof(mic_open));
	CHECK(c.valid && c.opcode == ATVV_CMD_MIC_OPEN, "mic open");
	c = atvv_parse_cmd(mic_close, sizeof(mic_close));
	CHECK(c.valid && c.opcode == ATVV_CMD_MIC_CLOSE && c.stream_id == 5, "mic close");
	c = atvv_parse_cmd(extend, sizeof(extend));
	CHECK(c.valid && c.opcode == ATVV_CMD_MIC_EXTEND && c.stream_id == 5, "extend");
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
	char events[32][16];
	int n;
	bool audio_on;
	bool watchdog_on;
};

static void log_ev(struct sm_log *l, const char *s)
{
	if (l->n < 32) {
		snprintf(l->events[l->n++], 16, "%s", s);
	}
}

static void t_send_ctl(void *ctx, const uint8_t *d, size_t len)
{
	char s[16];

	snprintf(s, sizeof(s), "ctl%02X", d[0]);
	log_ev(ctx, s);
}

static void t_hid(void *ctx, bool p)
{
	log_ev(ctx, p ? "keydown" : "keyup");
}

static void t_audio_start(void *ctx, uint8_t codec)
{
	((struct sm_log *)ctx)->audio_on = true;
	log_ev(ctx, "audio+");
}

static void t_audio_stop(void *ctx)
{
	((struct sm_log *)ctx)->audio_on = false;
	log_ev(ctx, "audio-");
}

static void t_watchdog(void *ctx, bool arm)
{
	((struct sm_log *)ctx)->watchdog_on = arm;
}

static const struct remote_sm_ops t_ops = {t_send_ctl, t_hid, t_audio_start, t_audio_stop,
					   t_watchdog};

static bool expect_seq(struct sm_log *l, const char *const *seq, int n)
{
	if (l->n != n) {
		return false;
	}
	for (int i = 0; i < n; i++) {
		if (strcmp(l->events[i], seq[i]) != 0) {
			return false;
		}
	}
	return true;
}

static void dump(struct sm_log *l)
{
	printf("    got:");
	for (int i = 0; i < l->n; i++) {
		printf(" %s", l->events[i]);
	}
	printf("\n");
}

static void test_sm_hold_to_talk(void)
{
	struct remote_sm sm;
	struct sm_log log = {0};
	const uint8_t get_caps[] = {0x0A, 0x01, 0x00, 0x00, 0x03, 0x03};
	const uint8_t mic_open[] = {0x0C, 0x00};

	remote_sm_init(&sm, &t_ops, &log, 120);
	remote_sm_host_write(&sm, get_caps, sizeof(get_caps));
	remote_sm_button(&sm, true);
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	CHECK(log.watchdog_on, "watchdog not armed on stream start");
	remote_sm_host_write(&sm, (const uint8_t[]){0x0E, sm.stream_id}, 2);
	remote_sm_button(&sm, false);

	/* AUDIO_STOP must precede HID key-up (main.swift relies on it). */
	const char *const want[] = {"ctl0B", "keydown", "ctl08", "ctl04", "audio+",
				    "audio-", "ctl00", "keyup"};
	bool ok = expect_seq(&log, want, 8);

	CHECK(ok, "hold-to-talk sequence");
	if (!ok) {
		dump(&log);
	}
	CHECK(!log.audio_on && !log.watchdog_on, "stream not fully stopped");
}

static void test_sm_short_tap_and_close(void)
{
	struct remote_sm sm;
	struct sm_log log = {0};
	const uint8_t mic_open[] = {0x0C, 0x00};

	remote_sm_init(&sm, &t_ops, &log, 120);

	/* Short tap: MIC_OPEN arrives after release -> error, no audio. */
	remote_sm_button(&sm, true);
	remote_sm_button(&sm, false);
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	const char *const tap[] = {"keydown", "ctl08", "keyup", "ctl0C"};
	bool ok = expect_seq(&log, tap, 4);

	CHECK(ok, "short tap sequence");
	if (!ok) {
		dump(&log);
	}

	/* Host MIC_CLOSE with the wrong stream id is ignored, right id stops. */
	log.n = 0;
	remote_sm_button(&sm, true);
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	uint8_t id = sm.stream_id;

	CHECK(id >= 1 && id <= 0x7F, "stream id range %u", id);
	remote_sm_host_write(&sm, (const uint8_t[]){0x0D, (uint8_t)(id + 1)}, 2);
	CHECK(log.audio_on, "wrong-id MIC_CLOSE stopped the stream");
	remote_sm_host_write(&sm, (const uint8_t[]){0x0D, id}, 2);
	CHECK(!log.audio_on, "MIC_CLOSE did not stop the stream");

	/* Timeout while held, then disconnect releases the key silently. */
	remote_sm_host_write(&sm, mic_open, sizeof(mic_open));
	CHECK(sm.stream_id != id, "stream id not advanced");
	remote_sm_timeout(&sm);
	CHECK(!log.audio_on, "timeout did not stop audio");
	log.n = 0;
	remote_sm_disconnected(&sm);
	const char *const disc[] = {"keyup"};

	ok = expect_seq(&log, disc, 1);
	CHECK(ok, "disconnect should only release the key");
	if (!ok) {
		dump(&log);
	}
}

static void test_sm_stream_id_wraps(void)
{
	struct remote_sm sm;
	struct sm_log log = {0};

	remote_sm_init(&sm, &t_ops, &log, 120);
	remote_sm_button(&sm, true);
	for (int i = 0; i < 300; i++) {
		log.n = 0;
		remote_sm_host_write(&sm, (const uint8_t[]){0x0C, 0x00}, 2);
		CHECK(sm.stream_id >= 1 && sm.stream_id <= 0x7F, "id %u out of range",
		      sm.stream_id);
		remote_sm_timeout(&sm);
	}
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
	printf("state machine\n");
	test_sm_hold_to_talk();
	test_sm_short_tap_and_close();
	test_sm_stream_id_wraps();

	if (failures) {
		printf("%d FAILED\n", failures);
		return 1;
	}
	printf("ALL PASS\n");
	return 0;
}

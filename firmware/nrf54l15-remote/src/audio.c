#include "audio.h"

#include <string.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_APP_AUDIO_SOURCE_DMIC)
#include <zephyr/audio/dmic.h>
#endif

#include "app.h"
#include "atvv_proto.h"
#include "atvv_service.h"

LOG_MODULE_REGISTER(audio, CONFIG_APP_LOG_LEVEL);

#define SAMPLE_RATE       16000
#define BLOCK_MS          10
#define BLOCK_SAMPLES     (SAMPLE_RATE * BLOCK_MS / 1000)
#define BLOCK_BYTES       (BLOCK_SAMPLES * sizeof(int16_t))

static struct atvv_framer framer;

/* ---- Framer output: runs on the audio thread. ---- */

static int send_audio(void *ctx, const uint8_t *data, size_t len)
{
	struct bt_conn *conn = app_conn_get();
	int err = conn ? 0 : -ENOTCONN;

	if (conn) {
		/* Backlog frames were cut before the MTU was known: split them if
		 * the link carries less (the host decodes a continuous stream). */
		size_t max = bt_gatt_get_mtu(conn) - 3;

		for (size_t off = 0; off < len && !err; off += max) {
			err = atvv_notify_audio(conn, &data[off], MIN(max, len - off));
		}
	}
	app_conn_put(conn);
	return err;
}

static int send_ctl(void *ctx, const uint8_t *data, size_t len)
{
	struct bt_conn *conn = app_conn_get();
	int err = atvv_notify_ctl(conn, data, len);

	app_conn_put(conn);
	return err;
}

static const struct atvv_framer_ops framer_ops = {
	.send_audio = send_audio,
	.send_ctl = send_ctl,
};

/* ---- Sources. ---- */

#if defined(CONFIG_APP_AUDIO_SOURCE_DMIC)

#define DMIC_BLOCK_COUNT 4
K_MEM_SLAB_DEFINE_STATIC(dmic_slab, BLOCK_BYTES, DMIC_BLOCK_COUNT, 4);

static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(pdm20));

static int source_init(void)
{
	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("PDM device not ready");
		return -ENODEV;
	}

	struct pcm_stream_cfg stream = {
		.pcm_width = 16,
		.mem_slab = &dmic_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan = 1,
		},
	};

	cfg.channel.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
	cfg.streams[0].pcm_rate = SAMPLE_RATE;
	cfg.streams[0].block_size = BLOCK_BYTES;

	return dmic_configure(dmic_dev, &cfg);
}

static int source_start(void)
{
	return dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
}

static void source_stop(void)
{
	(void)dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
}

/* Returns the number of samples copied into out, or a negative error. */
static int source_read(int16_t *out)
{
	void *block;
	size_t size;
	int err = dmic_read(dmic_dev, 0, &block, &size, 100);

	if (err) {
		return err;
	}

	size_t samples = MIN(size, BLOCK_BYTES) / sizeof(int16_t);

	memcpy(out, block, samples * sizeof(int16_t));
	k_mem_slab_free(&dmic_slab, block);

#if CONFIG_APP_MIC_GAIN_SHIFT > 0
	for (size_t i = 0; i < samples; i++) {
		int32_t v = (int32_t)out[i] << CONFIG_APP_MIC_GAIN_SHIFT;

		out[i] = (int16_t)CLAMP(v, INT16_MIN, INT16_MAX);
	}
#endif
	return (int)samples;
}

#else /* CONFIG_APP_AUDIO_SOURCE_TONE */

/* One period of a 1 kHz sine at 16 kHz, amplitude ~3000 (about -21 dBFS).
 * The Mac bridge adds +20 dB to remote audio, so this lands just below full
 * scale on the Mac side. */
static const int16_t sine_1khz[16] = {
	0,     1148,  2121,  2772,  3000,  2772,  2121,  1148,
	0,     -1148, -2121, -2772, -3000, -2772, -2121, -1148,
};
static int64_t next_block_ms;

static int source_init(void)
{
	return 0;
}

static int source_start(void)
{
	next_block_ms = k_uptime_get();
	return 0;
}

static void source_stop(void)
{
}

static int source_read(int16_t *out)
{
	/* Pace output at real time, like a microphone would. */
	next_block_ms += BLOCK_MS;
	k_sleep(K_TIMEOUT_ABS_MS(next_block_ms));

	for (size_t i = 0; i < BLOCK_SAMPLES; i++) {
		out[i] = sine_1khz[i % ARRAY_SIZE(sine_1khz)];
	}
	return BLOCK_SAMPLES;
}

#endif

/* ---- Thread. ---- */

enum audio_cmd_type {
	CMD_START,     /* live stream */
	CMD_PREBUFFER, /* record into the backlog */
	CMD_FREEZE,    /* stop recording, keep the backlog */
	CMD_RELEASE,   /* after a delay, send the backlog (then live) */
	CMD_STOP,
};

struct audio_cmd {
	uint8_t type;
	uint16_t frame_size;
	uint32_t delay_ms;
};

K_MSGQ_DEFINE(audio_cmds, sizeof(struct audio_cmd), 8, 4);

#if defined(CONFIG_APP_WAKE_CAPTURE)
#define BACKLOG_FRAMES                                                                             \
	(CONFIG_APP_WAKE_BUFFER_MS * 8 / CONFIG_APP_ATVV_FRAME_SIZE + 1)
static uint8_t backlog_mem[BACKLOG_FRAMES * ATVV_BACKLOG_SLOT_SIZE(CONFIG_APP_ATVV_FRAME_SIZE)];
#endif

static audio_backlog_sent_cb_t backlog_sent_cb;
/* ADPCM bytes in the backlog, readable from any thread (8 bytes per ms). */
static atomic_t backlog_bytes;

/* Thread-owned state. */
static bool recording;
static int64_t release_at; /* 0 = no release pending */

static void end_recording(void)
{
	if (recording) {
		source_stop();
		recording = false;
	}
}

static void log_stats(const char *what)
{
	LOG_INF("%s: %u frames sent, %u dropped, %u backlog frames dropped", what,
		framer.frames_sent, framer.frames_dropped, framer.backlog_dropped);
}

static void begin_recording(uint16_t frame_size, bool hold)
{
	end_recording();
	release_at = 0;
	atvv_framer_start(&framer, ATVV_CODEC_ADPCM_16K, frame_size);
#if defined(CONFIG_APP_WAKE_CAPTURE)
	if (hold) {
		atvv_framer_hold(&framer, backlog_mem, sizeof(backlog_mem));
	}
#endif
	atomic_set(&backlog_bytes, 0);

	int err = source_start();

	if (err) {
		LOG_ERR("audio source start failed: %d", err);
		return;
	}
	recording = true;
	LOG_INF("%s, %u-byte frames", hold ? "recording into backlog" : "stream started",
		frame_size);
}

static void handle_cmd(const struct audio_cmd *cmd)
{
	switch (cmd->type) {
	case CMD_START:
		begin_recording(cmd->frame_size, false);
		break;
	case CMD_PREBUFFER:
		begin_recording(CONFIG_APP_ATVV_FRAME_SIZE, true);
		break;
	case CMD_FREEZE:
		if (recording && framer.holding) {
			end_recording();
			atvv_framer_flush_partial(&framer);
			atomic_set(&backlog_bytes, atvv_framer_backlog_bytes(&framer));
		}
		break;
	case CMD_RELEASE:
		release_at = k_uptime_get() + cmd->delay_ms;
		if (release_at == 0) {
			release_at = 1;
		}
		break;
	case CMD_STOP:
		if (recording || framer.holding || release_at) {
			end_recording();
			log_stats("stream stopped");
		}
		framer.holding = false;
		framer.count = 0;
		release_at = 0;
		atomic_set(&backlog_bytes, 0);
		break;
	default:
		break;
	}
}

static void release_backlog(void)
{
	size_t bytes = atvv_framer_backlog_bytes(&framer);

	release_at = 0;
	atvv_framer_release(&framer);
	atomic_set(&backlog_bytes, 0);
	LOG_INF("backlog sent: %u ms of audio", (unsigned int)(bytes / 8));

	if (!recording) {
		log_stats("replay finished");
	}
	if (backlog_sent_cb) {
		backlog_sent_cb(recording);
	}
}

static void audio_thread(void *p1, void *p2, void *p3)
{
	static int16_t pcm[BLOCK_SAMPLES];

	for (;;) {
		struct audio_cmd cmd;
		k_timeout_t wait;

		if (recording) {
			wait = K_NO_WAIT;
		} else if (release_at) {
			wait = K_TIMEOUT_ABS_MS(release_at);
		} else {
			wait = K_FOREVER;
		}

		if (k_msgq_get(&audio_cmds, &cmd, wait) == 0) {
			handle_cmd(&cmd);
			continue;
		}

		if (release_at && k_uptime_get() >= release_at) {
			release_backlog();
		}

		if (recording) {
			int n = source_read(pcm);

			if (n < 0) {
				LOG_WRN("audio read error: %d", n);
				continue;
			}
			atvv_framer_push(&framer, pcm, (size_t)n);
			if (framer.holding) {
				atomic_set(&backlog_bytes, atvv_framer_backlog_bytes(&framer));
			}
		}
	}
}

K_THREAD_DEFINE(audio_tid, CONFIG_APP_AUDIO_THREAD_STACK_SIZE, audio_thread, NULL, NULL, NULL,
		CONFIG_APP_AUDIO_THREAD_PRIORITY, 0, 0);

static void send_cmd(const struct audio_cmd *cmd)
{
	if (k_msgq_put(&audio_cmds, cmd, K_NO_WAIT)) {
		LOG_ERR("audio command queue full, dropped %u", cmd->type);
	}
}

int audio_init(audio_backlog_sent_cb_t on_backlog_sent)
{
	backlog_sent_cb = on_backlog_sent;
	atvv_framer_init(&framer, &framer_ops, NULL);
	return source_init();
}

void audio_start(uint8_t codec, uint16_t frame_size)
{
	ARG_UNUSED(codec); /* only ADPCM 16 kHz is advertised */
	send_cmd(&(struct audio_cmd){.type = CMD_START, .frame_size = frame_size});
}

void audio_stop(void)
{
	send_cmd(&(struct audio_cmd){.type = CMD_STOP});
}

void audio_prebuffer_start(void)
{
	send_cmd(&(struct audio_cmd){.type = CMD_PREBUFFER});
}

void audio_prebuffer_freeze(void)
{
	send_cmd(&(struct audio_cmd){.type = CMD_FREEZE});
}

void audio_prebuffer_release(uint32_t delay_ms)
{
	send_cmd(&(struct audio_cmd){.type = CMD_RELEASE, .delay_ms = delay_ms});
}

uint32_t audio_backlog_ms(void)
{
	return (uint32_t)atomic_get(&backlog_bytes) / 8;
}

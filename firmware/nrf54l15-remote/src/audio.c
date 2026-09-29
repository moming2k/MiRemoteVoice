#include "audio.h"

#include <string.h>
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

static K_SEM_DEFINE(start_sem, 0, 1);
/* Incremented by every audio_start(); the thread ends a stream as soon as
 * the generation it is serving is no longer current or active is cleared. */
static atomic_t generation;
static atomic_t active;
static atomic_t stream_frame_size;

static struct atvv_framer framer;

/* ---- Framer output: runs on the audio thread. ---- */

static int send_audio(void *ctx, const uint8_t *data, size_t len)
{
	struct bt_conn *conn = app_conn_get();
	int err = atvv_notify_audio(conn, data, len);

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

static void audio_thread(void *p1, void *p2, void *p3)
{
	static int16_t pcm[BLOCK_SAMPLES];

	for (;;) {
		k_sem_take(&start_sem, K_FOREVER);

		atomic_val_t gen = atomic_get(&generation);

		if (!atomic_get(&active)) {
			continue;
		}

		uint16_t frame_size = (uint16_t)atomic_get(&stream_frame_size);

		atvv_framer_start(&framer, ATVV_CODEC_ADPCM_16K, frame_size);

		int err = source_start();

		if (err) {
			LOG_ERR("audio source start failed: %d", err);
			continue;
		}
		LOG_INF("stream started, %u-byte frames", frame_size);

		while (atomic_get(&active) && atomic_get(&generation) == gen) {
			int n = source_read(pcm);

			if (n < 0) {
				LOG_WRN("audio read error: %d", n);
				continue;
			}
			atvv_framer_push(&framer, pcm, (size_t)n);
		}

		source_stop();
		LOG_INF("stream stopped: %u frames sent, %u dropped", framer.frames_sent,
			framer.frames_dropped);

		/* A new stream may have been requested while this one ran. */
		if (atomic_get(&active)) {
			k_sem_give(&start_sem);
		}
	}
}

K_THREAD_DEFINE(audio_tid, CONFIG_APP_AUDIO_THREAD_STACK_SIZE, audio_thread, NULL, NULL, NULL,
		CONFIG_APP_AUDIO_THREAD_PRIORITY, 0, 0);

int audio_init(void)
{
	atvv_framer_init(&framer, &framer_ops, NULL);
	return source_init();
}

void audio_start(uint8_t codec, uint16_t frame_size)
{
	ARG_UNUSED(codec); /* only ADPCM 16 kHz is advertised */
	atomic_set(&stream_frame_size, frame_size);
	atomic_inc(&generation);
	atomic_set(&active, 1);
	k_sem_give(&start_sem);
}

void audio_stop(void)
{
	atomic_set(&active, 0);
}

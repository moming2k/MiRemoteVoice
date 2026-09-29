/*
 * MiRemoteVoice nRF54L15 voice remote.
 *
 * A BLE HID keyboard (voice key = F5) plus the Android TV Voice (ATVV) service
 * streaming 16 kHz IMA ADPCM, compatible with mi-remote-bridge on macOS.
 *
 * Threads:
 *   main  - owns the remote_sm state machine; all events arrive via app_events
 *   audio - reads the mic (or test tone), encodes and notifies audio frames
 */
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>

#include "app.h"
#include "atvv_proto.h"
#include "atvv_service.h"
#include "audio.h"
#include "hid.h"
#include "remote_sm.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

/* ---- Connection tracking. ---- */

static struct k_spinlock conn_lock;
static struct bt_conn *current_conn;

struct bt_conn *app_conn_get(void)
{
	k_spinlock_key_t key = k_spin_lock(&conn_lock);
	struct bt_conn *conn = current_conn ? bt_conn_ref(current_conn) : NULL;

	k_spin_unlock(&conn_lock, key);
	return conn;
}

void app_conn_put(struct bt_conn *conn)
{
	if (conn) {
		bt_conn_unref(conn);
	}
}

/* ---- Events for the main thread. ---- */

enum app_event_type {
	EVT_BUTTON,
	EVT_HOST_WRITE,
	EVT_TIMEOUT,
	EVT_DISCONNECTED,
};

struct app_event {
	uint8_t type;
	uint8_t len;
	uint8_t data[8]; /* EVT_BUTTON: data[0] = pressed; EVT_HOST_WRITE: payload */
};

K_MSGQ_DEFINE(app_events, sizeof(struct app_event), 16, 4);

static void post_event(const struct app_event *evt)
{
	if (k_msgq_put(&app_events, evt, K_NO_WAIT)) {
		LOG_WRN("event queue full, dropped type %u", evt->type);
	}
}

/* ---- Voice button (debounced GPIO). ---- */

#if DT_NODE_HAS_STATUS(DT_ALIAS(voice_button), okay)
#define BUTTON_NODE DT_ALIAS(voice_button)
#else
#define BUTTON_NODE DT_ALIAS(sw0)
#endif

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});
static struct gpio_callback button_cb;
static bool button_reported;

static void button_debounced(struct k_work *work)
{
	bool pressed = gpio_pin_get_dt(&button) > 0;

	if (pressed == button_reported) {
		return;
	}
	button_reported = pressed;
	post_event(&(struct app_event){.type = EVT_BUTTON, .data = {pressed}});
}

static K_WORK_DELAYABLE_DEFINE(button_work, button_debounced);

static void button_isr(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_reschedule(&button_work, K_MSEC(CONFIG_APP_BUTTON_DEBOUNCE_MS));
}

static int button_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&button)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (!err) {
		err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
	}
	if (err) {
		return err;
	}
	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	gpio_add_callback(button.port, &button_cb);

	if (led.port && gpio_is_ready_dt(&led)) {
		gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);
	}
	return 0;
}

static void set_led(bool on)
{
	if (led.port) {
		gpio_pin_set_dt(&led, on);
	}
}

/* ---- Stream watchdogs. ---- */

static void stream_timeout(struct k_work *work)
{
	post_event(&(struct app_event){.type = EVT_TIMEOUT});
}

/* Host must send MIC_EXTEND (the bridge does every 4 s) or the stream ends. */
static K_WORK_DELAYABLE_DEFINE(keepalive_work, stream_timeout);
/* Hard cap on stream length in case the button is stuck. */
static K_WORK_DELAYABLE_DEFINE(max_duration_work, stream_timeout);

/* ---- State machine glue (runs on the main thread). ---- */

static struct remote_sm sm;

static void sm_send_ctl(void *ctx, const uint8_t *data, size_t len)
{
	struct bt_conn *conn = app_conn_get();
	int err = atvv_notify_ctl(conn, data, len);

	if (err && conn) {
		LOG_WRN("ctl 0x%02x not sent: %d", data[0], err);
	}
	app_conn_put(conn);
}

static void sm_hid_key(void *ctx, bool pressed)
{
	struct bt_conn *conn = app_conn_get();
	int err = hid_voice_key(conn, pressed);

	if (err && conn) {
		LOG_WRN("HID key %s not sent: %d", pressed ? "down" : "up", err);
	}
	app_conn_put(conn);
}

static void sm_audio_start(void *ctx, uint8_t codec)
{
	k_work_reschedule(&max_duration_work, K_SECONDS(CONFIG_APP_STREAM_MAX_SECONDS));
	set_led(true);
	audio_start(codec);
}

static void sm_audio_stop(void *ctx)
{
	audio_stop();
	set_led(false);
	k_work_cancel_delayable(&max_duration_work);
}

static void sm_watchdog(void *ctx, bool arm)
{
	if (arm) {
		k_work_reschedule(&keepalive_work, K_SECONDS(CONFIG_APP_KEEPALIVE_TIMEOUT_SECONDS));
	} else {
		k_work_cancel_delayable(&keepalive_work);
	}
}

static const struct remote_sm_ops sm_ops = {
	.send_ctl = sm_send_ctl,
	.hid_key = sm_hid_key,
	.audio_start = sm_audio_start,
	.audio_stop = sm_audio_stop,
	.watchdog = sm_watchdog,
};

/* Bluetooth RX thread: copy the command and hand it to the main thread. */
static void on_atvv_write(const uint8_t *data, size_t len)
{
	struct app_event evt = {.type = EVT_HOST_WRITE};

	evt.len = (uint8_t)MIN(len, sizeof(evt.data));
	memcpy(evt.data, data, evt.len);
	post_event(&evt);
}

/* ---- Advertising and connections. ---- */

#define ATVV_SERVICE_UUID                                                                          \
	BT_UUID_128_ENCODE(ATVV_UUID_SERVICE_VAL, 0x5A21, 0x4F05, 0xBC7D, 0xAF01F617B664ULL)

/* The bridge finds the remote by the ATVV UUID in the advertisement. */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_GAP_APPEARANCE, (CONFIG_BT_DEVICE_APPEARANCE >> 0) & 0xff,
		      (CONFIG_BT_DEVICE_APPEARANCE >> 8) & 0xff),
	BT_DATA_BYTES(BT_DATA_UUID16_SOME, BT_UUID_16_ENCODE(BT_UUID_HIDS_VAL)),
	BT_DATA_BYTES(BT_DATA_UUID128_SOME, ATVV_SERVICE_UUID),
};

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void advertising_start(struct k_work *work)
{
	const struct bt_le_adv_param *param = BT_LE_ADV_PARAM(
		BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2, BT_GAP_ADV_FAST_INT_MAX_2, NULL);
	int err = bt_le_adv_start(param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err && err != -EALREADY) {
		LOG_ERR("advertising failed: %d", err);
		return;
	}
	LOG_INF("advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

static K_WORK_DEFINE(adv_work, advertising_start);

static void connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connection failed: 0x%02x", err);
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&conn_lock);

	if (current_conn) {
		/* CONFIG_BT_MAX_CONN=1 should make this unreachable. */
		k_spin_unlock(&conn_lock, key);
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}
	current_conn = bt_conn_ref(conn);
	k_spin_unlock(&conn_lock, key);

	LOG_INF("connected");
	hid_connected(conn);

	/* 7.5-15 ms while there is traffic; peripheral latency lets the radio
	 * sleep through idle intervals. */
	struct bt_le_conn_param param = BT_LE_CONN_PARAM_INIT(6, 12, 30, 400);

	bt_conn_le_param_update(conn, &param);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("disconnected: 0x%02x", reason);
	hid_disconnected(conn);

	k_spinlock_key_t key = k_spin_lock(&conn_lock);

	if (current_conn == conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	k_spin_unlock(&conn_lock, key);

	post_event(&(struct app_event){.type = EVT_DISCONNECTED});
}

static void recycled(void)
{
	k_work_submit(&adv_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err) {
		LOG_WRN("security failed: level %u err %d", level, err);
	} else {
		LOG_INF("security level %u", level);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

int main(void)
{
	int err;

	LOG_INF("MiRemoteVoice remote starting");

	err = button_init();
	if (err) {
		LOG_ERR("button init failed: %d", err);
		return 0;
	}

	err = audio_init();
	if (err) {
		LOG_ERR("audio init failed: %d", err);
		return 0;
	}

	remote_sm_init(&sm, &sm_ops, NULL, CONFIG_APP_ATVV_FRAME_SIZE);
	atvv_service_init(on_atvv_write);

	err = hid_init();
	if (err) {
		LOG_ERR("HID init failed: %d", err);
		return 0;
	}

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
		return 0;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		settings_load();
	}

	k_work_submit(&adv_work);

	for (;;) {
		struct app_event evt;

		k_msgq_get(&app_events, &evt, K_FOREVER);

		switch (evt.type) {
		case EVT_BUTTON:
			LOG_INF("voice button %s", evt.data[0] ? "down" : "up");
			remote_sm_button(&sm, evt.data[0]);
			break;
		case EVT_HOST_WRITE:
			LOG_DBG("host cmd 0x%02x (%u bytes)", evt.data[0], evt.len);
			remote_sm_host_write(&sm, evt.data, evt.len);
			break;
		case EVT_TIMEOUT:
			LOG_WRN("stream timeout");
			remote_sm_timeout(&sm);
			break;
		case EVT_DISCONNECTED:
			remote_sm_disconnected(&sm);
			break;
		default:
			break;
		}
	}
	return 0;
}

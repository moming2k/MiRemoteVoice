/*
 * MiRemoteVoice nRF54L15 voice remote.
 *
 * A BLE HID keyboard (voice key = F5) plus the Android TV Voice (ATVV) service
 * streaming 16 kHz IMA ADPCM, compatible with mi-remote-bridge on macOS.
 *
 * Threads:
 *   main  - owns the remote_sm state machine; all events arrive via app_events
 *   audio - reads the mic (or test tone), encodes and notifies audio frames
 *
 * Power: between presses the SoC idles in System ON sleep. While
 * disconnected it advertises fast, then slowly, and finally enters System OFF
 * (a few uA); the voice button wakes it with a reset. Audio spoken while it
 * reconnects is recorded and delivered once the host is ready (wake.h).
 */
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/settings/settings.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/poweroff.h>

#include "app.h"
#include "atvv_proto.h"
#include "atvv_service.h"
#include "audio.h"
#include "battery.h"
#include "hid.h"
#include "remote_sm.h"
#include "wake.h"

LOG_MODULE_REGISTER(app, CONFIG_APP_LOG_LEVEL);

#if defined(CONFIG_APP_PRODUCTION_BUILD)
BUILD_ASSERT(CONFIG_BT_DIS_PNP_VID != 0x1915 || CONFIG_BT_DIS_PNP_PID != 0xEEEF,
	     "Production build still uses Nordic's sample USB VID/PID: set "
	     "CONFIG_BT_DIS_PNP_VID/PID to your own IDs (see production.conf)");
#endif

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
	EVT_BUTTON,         /* data[0] = pressed */
	EVT_HOST_WRITE,     /* data = ATVV command */
	EVT_AUDIO_CCC,      /* data[0] = notifications enabled */
	EVT_TIMEOUT,        /* data[0] = enum remote_timeout */
	EVT_INACTIVE,       /* Active Remote Timeout expired */
	EVT_DISCONNECTED,
	EVT_SECURITY,       /* link encrypted */
	EVT_WAKE_TIMEOUT,   /* host not ready in time for the wake press */
	EVT_BACKLOG_SENT,   /* wake backlog sent (or abandoned) */
};

struct app_event {
	uint8_t type;
	uint8_t len;
	uint8_t data[8];
};

K_MSGQ_DEFINE(app_events, sizeof(struct app_event), 16, 4);

static void post_event(const struct app_event *evt)
{
	if (k_msgq_put(&app_events, evt, K_NO_WAIT)) {
		LOG_WRN("event queue full, dropped type %u", evt->type);
	}
}

/* ---- Voice button (debounced GPIO) and LED. ---- */

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
		/* A press that is still down at boot is the one that woke us. */
		button_reported = gpio_pin_get_dt(&button) > 0;

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

/* ---- Advertising and power policy (system workqueue). ---- */

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

static bool is_connected(void)
{
	struct bt_conn *conn = app_conn_get();
	bool connected = conn != NULL;

	app_conn_put(conn);
	return connected;
}

static void advertise(bool fast)
{
	/* Fast: quick reconnection right after boot, disconnect or a press.
	 * Slow: cheap background advertising until System OFF. */
	const struct bt_le_adv_param *param =
		fast ? BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_1,
				       BT_GAP_ADV_FAST_INT_MAX_1, NULL)
		     : BT_LE_ADV_PARAM(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_SLOW_INT_MIN,
				       BT_GAP_ADV_SLOW_INT_MAX, NULL);

	(void)bt_le_adv_stop();
	int err = bt_le_adv_start(param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("advertising failed: %d", err);
		return;
	}
	LOG_INF("advertising (%s)", fast ? "fast" : "slow");
}

static void adv_slow_handler(struct k_work *work)
{
	if (!is_connected()) {
		advertise(false);
	}
}

static K_WORK_DELAYABLE_DEFINE(adv_slow_work, adv_slow_handler);

static void poweroff_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(poweroff_work, poweroff_handler);

/* Start (or restart) the disconnected-state timeline: fast -> slow -> off. */
static void disconnected_timeline(struct k_work *work)
{
	if (is_connected()) {
		return;
	}
	advertise(true);
	k_work_reschedule(&adv_slow_work, K_SECONDS(CONFIG_APP_ADV_FAST_SECONDS));
	if (IS_ENABLED(CONFIG_APP_POWEROFF) && CONFIG_APP_POWEROFF_IDLE_SECONDS > 0) {
		k_work_reschedule(&poweroff_work, K_SECONDS(CONFIG_APP_POWEROFF_IDLE_SECONDS));
	}
}

static K_WORK_DEFINE(timeline_work, disconnected_timeline);

static void stop_disconnected_timeline(void)
{
	k_work_cancel_delayable(&adv_slow_work);
	k_work_cancel_delayable(&poweroff_work);
}

static void poweroff_handler(struct k_work *work)
{
#if defined(CONFIG_APP_POWEROFF)
	if (is_connected()) {
		return;
	}
	LOG_INF("idle while disconnected: entering System OFF, press the voice button to wake");
	(void)bt_le_adv_stop();
	audio_stop();
	set_led(false);

	/* Level-sensitive wake-up on the voice button. */
	int err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_LEVEL_ACTIVE);

	if (err) {
		LOG_ERR("cannot arm wake-up button (%d), staying on", err);
		gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH);
		return;
	}
	if (IS_ENABLED(CONFIG_LOG)) {
		log_panic(); /* flush pending log output */
	}
	sys_poweroff();
#endif
}

/* ---- Timers owned by the state machine glue. ---- */

static void transfer_timeout(struct k_work *work)
{
	post_event(&(struct app_event){.type = EVT_TIMEOUT, .data = {REMOTE_TIMEOUT_TRANSFER}});
}

static void max_duration_timeout(struct k_work *work)
{
	post_event(&(struct app_event){.type = EVT_TIMEOUT, .data = {REMOTE_TIMEOUT_MAX}});
}

static void active_timeout(struct k_work *work)
{
	post_event(&(struct app_event){.type = EVT_INACTIVE});
}

/* Spec "Audio Transfer Timeout": reset by AUDIO_START and MIC_EXTEND. */
static K_WORK_DELAYABLE_DEFINE(transfer_work, transfer_timeout);
/* Hard cap on stream length in case the button is stuck. */
static K_WORK_DELAYABLE_DEFINE(max_duration_work, max_duration_timeout);
/* Spec "Active Remote Timeout": MIC_OPEN only shortly after user activity. */
static K_WORK_DELAYABLE_DEFINE(active_work, active_timeout);

static void wake_timeout_handler(struct k_work *work)
{
	post_event(&(struct app_event){.type = EVT_WAKE_TIMEOUT});
}

static K_WORK_DELAYABLE_DEFINE(wake_timeout_work, wake_timeout_handler);

/* ---- State machine glue (runs on the main thread). ---- */

static struct remote_sm sm;
static struct wake wake;
/* The next AUDIO_START belongs to the wake press: send its backlog instead
 * of starting a fresh recording. */
static bool wake_stream_pending;
/* The wake backlog is waiting to be sent (route delay). */
static bool wake_backlog_inflight;
/* The button is already up: release the delivered press once the backlog has
 * been sent. */
static bool wake_replaying;

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

static void sm_audio_start(void *ctx, uint8_t codec, uint16_t frame_size)
{
	k_work_reschedule(&max_duration_work, K_SECONDS(CONFIG_APP_STREAM_MAX_SECONDS));
	set_led(true);
	if (wake_stream_pending) {
		wake_stream_pending = false;
		/* Hold the backlog back until the bridge has classified the press
		 * as a long press and routed the remote mic (300 ms); audio before
		 * that would be dropped on the Mac. */
		wake_backlog_inflight = true;
		audio_prebuffer_release(CONFIG_APP_WAKE_ROUTE_DELAY_MS);
	} else {
		audio_start(codec, frame_size);
	}
}

static void sm_audio_stop(void *ctx)
{
	if (wake_backlog_inflight) {
		/* Stream ended (timeout, MIC_CLOSE...) before the backlog went
		 * out: the audio thread will not report it, so do it here so a
		 * replayed press still gets released. */
		post_event(&(struct app_event){.type = EVT_BACKLOG_SENT});
	}
	audio_stop();
	set_led(false);
	k_work_cancel_delayable(&max_duration_work);
}

static void sm_transfer_timer(void *ctx, bool arm)
{
	if (arm) {
		k_work_reschedule(&transfer_work,
				  K_SECONDS(CONFIG_APP_AUDIO_TRANSFER_TIMEOUT_SECONDS));
	} else {
		k_work_cancel_delayable(&transfer_work);
	}
}

static void sm_user_activity(void *ctx)
{
	if (CONFIG_APP_ACTIVE_REMOTE_TIMEOUT_SECONDS > 0) {
		k_work_reschedule(&active_work, K_SECONDS(CONFIG_APP_ACTIVE_REMOTE_TIMEOUT_SECONDS));
	}
}

static const struct remote_sm_ops sm_ops = {
	.send_ctl = sm_send_ctl,
	.hid_key = sm_hid_key,
	.audio_start = sm_audio_start,
	.audio_stop = sm_audio_stop,
	.transfer_timer = sm_transfer_timer,
	.user_activity = sm_user_activity,
};

/* Keep the state machine's view of the link current before each event. */
static void refresh_link_state(void)
{
	struct bt_conn *conn = app_conn_get();

	if (conn) {
		uint16_t mtu = bt_gatt_get_mtu(conn);
		uint16_t max = CONFIG_APP_ATVV_FRAME_SIZE;

		if (mtu > 3 && mtu - 3 < max) {
			max = mtu - 3;
		}
		remote_sm_set_max_frame_size(&sm, max);
		remote_sm_audio_subscribed(&sm, atvv_audio_subscribed(conn));
	}
	app_conn_put(conn);
}

/* Bluetooth RX thread: copy the command and hand it to the main thread. */
static void on_atvv_write(const uint8_t *data, size_t len)
{
	struct app_event evt = {.type = EVT_HOST_WRITE};

	evt.len = (uint8_t)MIN(len, sizeof(evt.data));
	memcpy(evt.data, data, evt.len);
	post_event(&evt);
}

static void on_audio_ccc(bool enabled)
{
	post_event(&(struct app_event){.type = EVT_AUDIO_CCC, .data = {enabled}});
}

/* Audio thread: the wake backlog has been sent. */
static void on_backlog_sent(bool recording)
{
	ARG_UNUSED(recording);
	post_event(&(struct app_event){.type = EVT_BACKLOG_SENT});
}

/* ---- Wake press (runs on the main thread). ---- */

static const char *const wake_action_names[] = {
	"none", "freeze", "restart", "deliver hold", "deliver replay", "deliver press", "discard",
};

/* Deliver the wake press to the state machine as a (replayed) press. */
static void deliver_wake_press(bool replay)
{
	wake_stream_pending = true;
	wake_replaying = replay;
	remote_sm_button(&sm, true);

	if (wake_stream_pending) {
		/* No stream started (e.g. notifications went away): give up. */
		wake_stream_pending = false;
		audio_stop();
		if (replay) {
			wake_replaying = false;
			remote_sm_button(&sm, false);
		}
	}
}

static void apply_wake_action(enum wake_action action)
{
	if (action == WAKE_NONE) {
		return;
	}
	LOG_INF("wake press: %s (backlog %u ms)", wake_action_names[action], audio_backlog_ms());

	switch (action) {
	case WAKE_FREEZE:
		audio_prebuffer_freeze();
		break;
	case WAKE_RESTART:
		audio_prebuffer_start();
		break;
	case WAKE_DELIVER_HOLD:
		deliver_wake_press(false);
		break;
	case WAKE_DELIVER_REPLAY:
		deliver_wake_press(true);
		break;
	case WAKE_DELIVER_PRESS:
		audio_stop();
		remote_sm_button(&sm, true);
		break;
	case WAKE_DISCARD:
		audio_stop();
		break;
	default:
		break;
	}

	if (!wake_pending(&wake)) {
		k_work_cancel_delayable(&wake_timeout_work);
	}
}

/* Deliver the wake press once the host can take it. */
static void check_wake_ready(void)
{
	if (!wake_pending(&wake)) {
		return;
	}

	struct bt_conn *conn = app_conn_get();
	bool ready = conn && bt_conn_get_security(conn) >= BT_SECURITY_L2 && sm.caps_received &&
		     sm.audio_subscribed;

	app_conn_put(conn);
	if (ready) {
		apply_wake_action(wake_ready(&wake, sm.model == ATVV_MODEL_HOLD_TO_TALK,
					     audio_backlog_ms()));
	}
}

/* ---- Connection callbacks. ---- */

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
	stop_disconnected_timeline();
	hid_connected(conn);
	battery_measure();

	/* 7.5-15 ms while there is traffic; peripheral latency lets the radio
	 * skip up to 30 idle intervals between presses. */
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
	k_work_submit(&timeline_work);
}

static void security_changed(struct bt_conn *conn, bt_security_t level, enum bt_security_err err)
{
	if (err) {
		LOG_WRN("security failed: level %u err %d", level, err);
	} else {
		LOG_INF("security level %u", level);
		post_event(&(struct app_event){.type = EVT_SECURITY});
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = recycled,
	.security_changed = security_changed,
};

static void handle_event(const struct app_event *evt)
{
	switch (evt->type) {
	case EVT_BUTTON: {
		enum wake_action action;

		LOG_INF("voice button %s", evt->data[0] ? "down" : "up");
		if (evt->data[0] && !is_connected()) {
			/* Pressing while disconnected: advertise fast again and
			 * postpone System OFF. */
			k_work_submit(&timeline_work);
		}
		if (wake_button(&wake, evt->data[0], &action)) {
			apply_wake_action(action);
			break;
		}
		if (!evt->data[0] && wake_backlog_inflight && !wake_replaying) {
			/* Released during the route delay: stop recording, send what
			 * was recorded, then release (EVT_BACKLOG_SENT). */
			audio_prebuffer_freeze();
			wake_replaying = true;
			break;
		}
		refresh_link_state();
		remote_sm_button(&sm, evt->data[0]);
		break;
	}
	case EVT_HOST_WRITE:
		LOG_DBG("host cmd 0x%02x (%u bytes)", evt->data[0], evt->len);
		refresh_link_state();
		remote_sm_host_write(&sm, evt->data, evt->len);
		check_wake_ready();
		break;
	case EVT_AUDIO_CCC:
		remote_sm_audio_subscribed(&sm, evt->data[0]);
		check_wake_ready();
		break;
	case EVT_SECURITY:
		refresh_link_state();
		check_wake_ready();
		break;
	case EVT_WAKE_TIMEOUT:
		apply_wake_action(wake_timeout(&wake));
		break;
	case EVT_BACKLOG_SENT:
		wake_backlog_inflight = false;
		if (wake_replaying) {
			wake_replaying = false;
			remote_sm_button(&sm, false);
		}
		break;
	case EVT_TIMEOUT:
		LOG_WRN("stream timeout (%s)",
			evt->data[0] == REMOTE_TIMEOUT_TRANSFER ? "no MIC_EXTEND" : "max duration");
		remote_sm_timeout(&sm, evt->data[0]);
		break;
	case EVT_INACTIVE:
		remote_sm_set_active(&sm, false);
		break;
	case EVT_DISCONNECTED:
		wake_stream_pending = false;
		wake_backlog_inflight = false;
		wake_replaying = false;
		remote_sm_disconnected(&sm);
		break;
	default:
		break;
	}
}

int main(void)
{
	int err;

	LOG_INF("MiRemoteVoice remote starting");

	err = button_init();
	if (err) {
		LOG_ERR("button init failed: %d", err);
		return 0;
	}

	err = audio_init(on_backlog_sent);
	if (err) {
		LOG_ERR("audio init failed: %d", err);
		return 0;
	}

	wake_init(&wake, IS_ENABLED(CONFIG_APP_WAKE_CAPTURE) && button_reported,
		  CONFIG_APP_WAKE_MIN_REPLAY_MS);
	if (wake_pending(&wake)) {
		LOG_INF("woken by the voice button: recording until the host is ready");
		audio_prebuffer_start();
		k_work_reschedule(&wake_timeout_work, K_SECONDS(CONFIG_APP_WAKE_TIMEOUT_SECONDS));
	}

	remote_sm_init(&sm, &sm_ops, NULL);
	if (CONFIG_APP_ACTIVE_REMOTE_TIMEOUT_SECONDS == 0) {
		remote_sm_set_active(&sm, true); /* timeout disabled */
	}
	atvv_service_init(on_atvv_write, on_audio_ccc);

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

	/* Battery reporting is optional; keep going without it. */
	(void)battery_init();

	k_work_submit(&timeline_work);

	for (;;) {
		struct app_event evt;

		k_msgq_get(&app_events, &evt, K_FOREVER);
		handle_event(&evt);
	}
	return 0;
}

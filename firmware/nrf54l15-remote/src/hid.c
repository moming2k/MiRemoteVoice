/*
 * Boot-compatible HID keyboard. Only one key is ever reported: HID usage 0x3E
 * (F5), which macOS delivers as virtual key code 0x60 — the key
 * mi-remote-bridge intercepts (VK.voiceKey in main.swift).
 *
 * The report map and service setup follow nRF Connect SDK's
 * peripheral_hids_keyboard sample.
 */
#include "hid.h"

#include <string.h>
#include <zephyr/logging/log.h>
#include <bluetooth/services/hids.h>

LOG_MODULE_REGISTER(hid, CONFIG_APP_LOG_LEVEL);

#define BASE_USB_HID_SPEC_VERSION 0x0101
#define INPUT_REPORT_KEYS_MAX_LEN 8 /* modifiers, reserved, 6 key codes */
#define OUTPUT_REPORT_MAX_LEN     1 /* LED state from the host (ignored) */
#define INPUT_REP_KEYS_IDX        0
#define OUTPUT_REP_KEYS_IDX       0
#define HID_USAGE_F5              0x3E

BT_HIDS_DEF(hids_obj, OUTPUT_REPORT_MAX_LEN, INPUT_REPORT_KEYS_MAX_LEN);

static bool in_boot_mode;

static const uint8_t report_map[] = {
	0x05, 0x01, /* Usage Page (Generic Desktop) */
	0x09, 0x06, /* Usage (Keyboard) */
	0xA1, 0x01, /* Collection (Application) */

	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0xE0, /*   Usage Minimum (224) */
	0x29, 0xE7, /*   Usage Maximum (231) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x01, /*   Logical Maximum (1) */
	0x75, 0x01, /*   Report Size (1) */
	0x95, 0x08, /*   Report Count (8) */
	0x81, 0x02, /*   Input (Data, Variable, Absolute): modifiers */

	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x08, /*   Report Size (8) */
	0x81, 0x01, /*   Input (Constant): reserved */

	0x95, 0x06, /*   Report Count (6) */
	0x75, 0x08, /*   Report Size (8) */
	0x15, 0x00, /*   Logical Minimum (0) */
	0x25, 0x65, /*   Logical Maximum (101) */
	0x05, 0x07, /*   Usage Page (Key Codes) */
	0x19, 0x00, /*   Usage Minimum (0) */
	0x29, 0x65, /*   Usage Maximum (101) */
	0x81, 0x00, /*   Input (Data, Array): key array */

	0x95, 0x05, /*   Report Count (5) */
	0x75, 0x01, /*   Report Size (1) */
	0x05, 0x08, /*   Usage Page (LEDs) */
	0x19, 0x01, /*   Usage Minimum (1) */
	0x29, 0x05, /*   Usage Maximum (5) */
	0x91, 0x02, /*   Output (Data, Variable, Absolute): LEDs */
	0x95, 0x01, /*   Report Count (1) */
	0x75, 0x03, /*   Report Size (3) */
	0x91, 0x01, /*   Output (Constant): LED padding */

	0xC0, /* End Collection */
};

static void outp_rep_handler(struct bt_hids_rep *rep, struct bt_conn *conn, bool write)
{
	/* Caps Lock etc. LED state: nothing to show on a voice remote. */
}

static void pm_evt_handler(enum bt_hids_pm_evt evt, struct bt_conn *conn)
{
	in_boot_mode = (evt == BT_HIDS_PM_EVT_BOOT_MODE_ENTERED);
	LOG_INF("HID %s mode", in_boot_mode ? "boot" : "report");
}

int hid_init(void)
{
	struct bt_hids_init_param init = {0};
	struct bt_hids_inp_rep *inp = &init.inp_rep_group_init.reports[INPUT_REP_KEYS_IDX];
	struct bt_hids_outp_feat_rep *outp = &init.outp_rep_group_init.reports[OUTPUT_REP_KEYS_IDX];

	init.rep_map.data = report_map;
	init.rep_map.size = sizeof(report_map);

	init.info.bcd_hid = BASE_USB_HID_SPEC_VERSION;
	init.info.b_country_code = 0x00;
	init.info.flags = BT_HIDS_REMOTE_WAKE | BT_HIDS_NORMALLY_CONNECTABLE;

	inp->size = INPUT_REPORT_KEYS_MAX_LEN;
	inp->id = 0;
	init.inp_rep_group_init.cnt++;

	outp->size = OUTPUT_REPORT_MAX_LEN;
	outp->id = 0;
	outp->handler = outp_rep_handler;
	init.outp_rep_group_init.cnt++;

	init.is_kb = true;
	init.boot_kb_outp_rep_handler = outp_rep_handler;
	init.pm_evt_handler = pm_evt_handler;

	return bt_hids_init(&hids_obj, &init);
}

void hid_connected(struct bt_conn *conn)
{
	in_boot_mode = false;

	int err = bt_hids_connected(&hids_obj, conn);

	if (err) {
		LOG_ERR("bt_hids_connected: %d", err);
	}
}

void hid_disconnected(struct bt_conn *conn)
{
	int err = bt_hids_disconnected(&hids_obj, conn);

	if (err) {
		LOG_ERR("bt_hids_disconnected: %d", err);
	}
}

int hid_voice_key(struct bt_conn *conn, bool pressed)
{
	uint8_t report[INPUT_REPORT_KEYS_MAX_LEN] = {0};

	if (!conn) {
		return -ENOTCONN;
	}
	if (pressed) {
		report[2] = HID_USAGE_F5;
	}

	if (in_boot_mode) {
		return bt_hids_boot_kb_inp_rep_send(&hids_obj, conn, report, sizeof(report), NULL);
	}
	return bt_hids_inp_rep_send(&hids_obj, conn, INPUT_REP_KEYS_IDX, report, sizeof(report),
				    NULL);
}

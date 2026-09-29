#include "atvv_service.h"

#include <errno.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/logging/log.h>

#include "atvv_proto.h"

LOG_MODULE_REGISTER(atvv, CONFIG_APP_LOG_LEVEL);

#define ATVV_UUID(val) BT_UUID_128_ENCODE(val, 0x5A21, 0x4F05, 0xBC7D, 0xAF01F617B664ULL)

static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(ATVV_UUID(ATVV_UUID_SERVICE_VAL));
static const struct bt_uuid_128 tx_uuid = BT_UUID_INIT_128(ATVV_UUID(ATVV_UUID_TX_VAL));
static const struct bt_uuid_128 audio_uuid = BT_UUID_INIT_128(ATVV_UUID(ATVV_UUID_AUDIO_VAL));
static const struct bt_uuid_128 ctl_uuid = BT_UUID_INIT_128(ATVV_UUID(ATVV_UUID_CTL_VAL));

static atvv_write_cb_t write_cb;

static ssize_t tx_write(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
			uint16_t len, uint16_t offset, uint8_t flags)
{
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (write_cb) {
		write_cb(buf, len);
	}
	return len;
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	LOG_DBG("CCC %p = 0x%04x", (void *)attr, value);
}

/*
 * Encryption is required so the Mac pairs once (as a keyboard) and every later
 * connection is bonded. Attribute indices below are used by the notify helpers.
 */
BT_GATT_SERVICE_DEFINE(atvv_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	/* [1,2] TX: host -> remote commands */
	BT_GATT_CHARACTERISTIC(&tx_uuid.uuid, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
			       BT_GATT_PERM_WRITE_ENCRYPT, NULL, tx_write, NULL),
	/* [3,4,5] AUDIO: remote -> host ADPCM */
	BT_GATT_CHARACTERISTIC(&audio_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
	/* [6,7,8] CTL: remote -> host control events */
	BT_GATT_CHARACTERISTIC(&ctl_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE_ENCRYPT),
);

#define AUDIO_ATTR (&atvv_svc.attrs[4])
#define CTL_ATTR   (&atvv_svc.attrs[7])

void atvv_service_init(atvv_write_cb_t on_write)
{
	write_cb = on_write;
}

static int notify(struct bt_conn *conn, const struct bt_gatt_attr *attr, const uint8_t *data,
		  size_t len)
{
	if (!conn) {
		return -ENOTCONN;
	}
	if (!bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY)) {
		return -EACCES;
	}
	return bt_gatt_notify(conn, attr, data, len);
}

int atvv_notify_ctl(struct bt_conn *conn, const uint8_t *data, size_t len)
{
	return notify(conn, CTL_ATTR, data, len);
}

int atvv_notify_audio(struct bt_conn *conn, const uint8_t *data, size_t len)
{
	return notify(conn, AUDIO_ATTR, data, len);
}

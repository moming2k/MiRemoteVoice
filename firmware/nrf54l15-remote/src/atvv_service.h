/* ATVV GATT service (UUID AB5E0001-...). */
#ifndef ATVV_SERVICE_H_
#define ATVV_SERVICE_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/bluetooth/conn.h>

/* Called from the Bluetooth RX thread for every write to the TX
 * characteristic. Must not block. */
typedef void (*atvv_write_cb_t)(const uint8_t *data, size_t len);

void atvv_service_init(atvv_write_cb_t on_write);

/* May block waiting for a TX buffer; never call from the system workqueue
 * expecting it to wait (it returns -ENOMEM there instead). */
int atvv_notify_ctl(struct bt_conn *conn, const uint8_t *data, size_t len);
int atvv_notify_audio(struct bt_conn *conn, const uint8_t *data, size_t len);

#endif /* ATVV_SERVICE_H_ */

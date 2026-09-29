/* HID-over-GATT keyboard exposing the voice key (F5) to macOS. */
#ifndef HID_H_
#define HID_H_

#include <stdbool.h>
#include <zephyr/bluetooth/conn.h>

int hid_init(void);
void hid_connected(struct bt_conn *conn);
void hid_disconnected(struct bt_conn *conn);
/* Press or release the voice key on the given connection. */
int hid_voice_key(struct bt_conn *conn, bool pressed);

#endif /* HID_H_ */

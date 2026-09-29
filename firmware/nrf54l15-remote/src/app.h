/* Shared application state. */
#ifndef APP_H_
#define APP_H_

#include <zephyr/bluetooth/conn.h>

/* Take a reference to the current connection (NULL if none). Always pair with
 * app_conn_put(), which accepts NULL. Safe from any thread. */
struct bt_conn *app_conn_get(void);
void app_conn_put(struct bt_conn *conn);

#endif /* APP_H_ */

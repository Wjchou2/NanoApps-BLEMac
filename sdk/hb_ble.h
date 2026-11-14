#ifndef HB_BLE_H
#define HB_BLE_H

#include <stdbool.h>
#include <stdint.h>

#define HB_BLE_SCAN_LIMIT 24u
#define HB_BLE_ADDRESS_SIZE 7u
#define HB_BLE_NAME_SIZE 32u
#define HB_BLE_NOTIFICATION_LIMIT 64u

typedef struct {
    uint8_t address[HB_BLE_ADDRESS_SIZE]; /* Type byte followed by the address. */
    char name[HB_BLE_NAME_SIZE];
    const char *type;
} hb_ble_device_t;

typedef bool (*hb_ble_filter_fn)(const hb_ble_device_t *device);

/* Scans for named peripherals for up to five seconds. The optional filter
 * runs under the firmware scan lock and must not call Bluetooth or the UI. */
const char *hb_ble_scan(hb_ble_device_t found[HB_BLE_SCAN_LIMIT],
                        unsigned *count, hb_ble_filter_fn filter);

#ifdef HB_BLE_UART
typedef struct {
    unsigned poll_ms, length;
    uint8_t data[HB_BLE_NOTIFICATION_LIMIT];
    const char *status;
    char text[96];
} hb_ble_update_t;

/* Connects to the scanned address and starts Nordic UART discovery. Use
 * hb_ble_connect for Nano UART Sender text/audio or connect_notifications
 * to receive binary TX notifications as-is. Poll on the UI task.
 * Each update contains the latest notification (up to 64 bytes), if any.
 * Status is optional and valid until the update object is reused. */
const char *hb_ble_connect(const uint8_t address[HB_BLE_ADDRESS_SIZE]);
const char *hb_ble_connect_notifications(const uint8_t address[HB_BLE_ADDRESS_SIZE]);
/* Connected includes service discovery. Ready means TX is subscribed. */
bool hb_ble_connected(void);
bool hb_ble_ready(void);
void hb_ble_poll(hb_ble_update_t *update);
void hb_ble_disconnect(void);
/* Call from prepare_unload and wait for a nonzero result before unloading. */
int hb_ble_prepare_unload(void);
#endif

#endif

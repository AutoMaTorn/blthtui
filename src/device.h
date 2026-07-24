#ifndef BLUETUI_DEVICE_H
#define BLUETUI_DEVICE_H

#include <stdbool.h>

/* Fixed sizes keep the model allocation-free and easy to store in arrays. */
#define BT_ADDR_LEN 18   /* "XX:XX:XX:XX:XX:XX" + NUL            */
#define BT_NAME_LEN 128  /* Alias/Name, truncated if longer      */
#define BT_PATH_LEN 128  /* D-Bus object path of the device      */

/* A snapshot of one org.bluez.Device1 object. */
typedef struct {
    char path[BT_PATH_LEN];   /* /org/bluez/hci0/dev_XX_XX_...   */
    char address[BT_ADDR_LEN];
    char name[BT_NAME_LEN];   /* Alias, falls back to Name/addr  */
    bool paired;
    bool connected;
    bool trusted;
    short rssi;               /* 0 when unknown                  */
    bool has_rssi;
} bt_device;

#endif /* BLUETUI_DEVICE_H */

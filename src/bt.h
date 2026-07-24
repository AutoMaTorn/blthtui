#ifndef BLTTUI_BT_H
#define BLTTUI_BT_H

#include <stdbool.h>
#include <stddef.h>
#include "device.h"

/* Opaque handle wrapping the sd-bus connection and the selected adapter. */
typedef struct bt_ctx bt_ctx;

/* Connect to the system bus and locate the first Bluetooth adapter.
 * Returns a heap-allocated context, or NULL on failure (message printed to
 * *err if non-NULL; caller must not free the string). */
bt_ctx *bt_open(const char **err);
void    bt_close(bt_ctx *ctx);

/* D-Bus object path of the selected adapter, e.g. "/org/bluez/hci0". */
const char *bt_adapter_path(const bt_ctx *ctx);

/* Adapter power state. bt_set_powered returns 0 on success, -errno on error. */
int bt_get_powered(bt_ctx *ctx, bool *out);
int bt_set_powered(bt_ctx *ctx, bool on);

/* Discovery (scanning). Return 0 on success, -errno on error. */
int bt_start_discovery(bt_ctx *ctx);
int bt_stop_discovery(bt_ctx *ctx);
int bt_get_discovering(bt_ctx *ctx, bool *out);

/* Enumerate every known device via ObjectManager.GetManagedObjects.
 * Fills up to `max` entries into `out`; returns the count, or -errno. */
int bt_list_devices(bt_ctx *ctx, bt_device *out, size_t max);

/* Per-device actions. `path` is bt_device.path. Return 0 / -errno. */
int bt_connect(bt_ctx *ctx, const char *path);
int bt_disconnect(bt_ctx *ctx, const char *path);
int bt_pair(bt_ctx *ctx, const char *path);
int bt_remove(bt_ctx *ctx, const char *path);

/* File descriptor to poll for incoming BlueZ signals. */
int bt_get_fd(bt_ctx *ctx);
/* Drain all pending bus traffic; call after poll() reports the fd readable.
 * Returns the number of messages processed, or -errno. */
int bt_process(bt_ctx *ctx);
/* True (and cleared) if a BlueZ signal has arrived since the last check,
 * i.e. the device list may have changed and should be refreshed. */
bool bt_take_dirty(bt_ctx *ctx);

/* ---- pairing agent (stage 6) ----
 *
 * The UI supplies these callbacks; BlueZ invokes them via our registered
 * org.bluez.Agent1 object whenever a pairing needs user interaction. They run
 * synchronously inside bt_process()/bt_pair(), where it is safe to draw. */
typedef struct {
    void *userdata;
    /* Numeric confirmation. passkey >= 0: "does 000000 match?"; passkey < 0:
     * a plain yes/no authorization. Return true to accept. */
    bool (*confirm)(void *ud, const char *dev, long passkey);
    /* Ask the user to type the numeric passkey shown on the peer. */
    bool (*request_passkey)(void *ud, const char *dev, unsigned *out);
    /* Ask the user to type a PIN string. */
    bool (*request_pin)(void *ud, const char *dev, char *buf, size_t sz);
    /* Show a passkey/PIN the user must enter on the peer (no reply). */
    void (*display)(void *ud, const char *dev, const char *what);
} bt_agent_cb;

/* Register as the default pairing agent. Returns 0 on success, -errno.
 * Failure to become the *default* agent is non-fatal and logged. */
int bt_register_agent(bt_ctx *ctx, const bt_agent_cb *cb);

#endif /* BLTTUI_BT_H */

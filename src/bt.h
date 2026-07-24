#ifndef BLTHTUI_BT_H
#define BLTHTUI_BT_H

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

/* D-Bus object path of the selected adapter, e.g. "/org/bluez/hci0", or an
 * empty string once the adapter has gone away (unplugged dongle). */
const char *bt_adapter_path(const bt_ctx *ctx);

/* True while live signal subscriptions are in place. When false the caller is
 * running blind on its own timer and should say so in the UI. */
bool bt_live_updates(const bt_ctx *ctx);

/* Make sure an adapter is selected, re-scanning if the previous one vanished.
 * Returns 0 when one is available, -ENODEV when none is. Cheap when the
 * current adapter is still valid: no bus traffic at all. */
int bt_ensure_adapter(bt_ctx *ctx);

/* ---- actions ----
 *
 * Each returns 0 on success or -errno. On failure, when `err` is non-NULL it
 * receives BlueZ's own error text ("br-connection-profile-unavailable"), which
 * is far more useful than strerror() on the errno; it falls back to strerror()
 * when BlueZ supplied no message. BT_ERR_LEN is a comfortable buffer size. */
#define BT_ERR_LEN 160

/* Adapter power state. */
int bt_get_powered(bt_ctx *ctx, bool *out);
int bt_set_powered(bt_ctx *ctx, bool on, char *err, size_t errsz);

/* Discovery (scanning). */
int bt_start_discovery(bt_ctx *ctx, char *err, size_t errsz);
int bt_stop_discovery(bt_ctx *ctx, char *err, size_t errsz);
int bt_get_discovering(bt_ctx *ctx, bool *out);

/* Enumerate every known device via ObjectManager.GetManagedObjects.
 * Fills up to `max` entries into `out`; returns the count, or -errno. */
int bt_list_devices(bt_ctx *ctx, bt_device *out, size_t max);

/* Per-device actions. `path` is bt_device.path; `err` as described above.
 * bt_pair runs with a long timeout, since BlueZ only answers once the whole
 * pairing exchange — including our agent's dialogs — has finished. */
int bt_connect(bt_ctx *ctx, const char *path, char *err, size_t errsz);
int bt_disconnect(bt_ctx *ctx, const char *path, char *err, size_t errsz);
int bt_pair(bt_ctx *ctx, const char *path, char *err, size_t errsz);
int bt_remove(bt_ctx *ctx, const char *path, char *err, size_t errsz);

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

#endif /* BLTHTUI_BT_H */

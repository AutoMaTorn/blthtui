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

/* Adapter power state. Reading is free: the value is kept up to date from
 * PropertiesChanged, so it costs no bus traffic. */
bool bt_powered(const bt_ctx *ctx);
int  bt_set_powered(bt_ctx *ctx, bool on, char *err, size_t errsz);

/* Discovery (scanning). bt_discovering() is likewise free. */
int  bt_start_discovery(bt_ctx *ctx, char *err, size_t errsz);
int  bt_stop_discovery(bt_ctx *ctx, char *err, size_t errsz);
bool bt_discovering(const bt_ctx *ctx);

/* ---- device model ----
 *
 * The context owns the device list and keeps it current from BlueZ signals;
 * GetManagedObjects is issued once at startup and then only to recover from a
 * gap (an unknown device, a re-acquired adapter, the slow safety resync).
 *
 * bt_devices() hands out the internal array — valid until the next
 * bt_process() — and re-sorts it only when the order can actually have
 * changed. An RSSI update alone never reorders the list, so rows do not slide
 * out from under the cursor while a scan is running. */
/* A busy café easily shows 100+ devices; 128 was close enough to the ceiling
 * to hit it, and overflow used to be silent. */
#define BT_MAX_DEVICES 256

int bt_devices(bt_ctx *ctx, const bt_device **out);

/* True when the model is full, so BlueZ may know devices we are not showing.
 * Overflow used to be silent; the UI says so now. */
bool bt_truncated(const bt_ctx *ctx);

/* Force a full re-read of the model. Only needed after something that
 * bypasses signals; ordinary use never has to call it. */
int bt_sync(bt_ctx *ctx);

int bt_remove(bt_ctx *ctx, const char *path, char *err, size_t errsz);
int bt_set_trusted(bt_ctx *ctx, const char *path, bool on, char *err, size_t errsz);

/* ---- asynchronous device actions ----
 *
 * Connect and Pair take seconds, and Pair is worse than slow: BlueZ answers it
 * only after our own agent has replied to whatever it asked. A blocking call
 * cannot do that — sd_bus_call queues incoming messages instead of dispatching
 * them, so the agent request would sit unanswered until the call timed out.
 * Running the call asynchronously keeps the event loop alive, which is what
 * lets the agent (and the UI) respond at all.
 *
 * One action at a time; starting another while one is in flight returns
 * -EBUSY. */
typedef enum { BT_ACT_CONNECT, BT_ACT_DISCONNECT, BT_ACT_PAIR } bt_action;

int bt_action_start(bt_ctx *ctx, const char *path, bt_action act,
                    char *err, size_t errsz);

/* Human-readable label of the action in flight ("Connecting"), or NULL. */
const char *bt_action_active(const bt_ctx *ctx);

/* True exactly once per finished action. `err` is empty on success. */
bool bt_action_result(bt_ctx *ctx, char *label, size_t labelsz,
                      char *err, size_t errsz);

/* ---- extra per-device detail, fetched on demand ---- */
typedef struct {
    bool has_battery;
    int  battery;              /* 0..100, valid when has_battery */
    char icon[32];             /* "audio-headset" and friends, "" if unknown */
    int  nuuid;
    char uuid[8][40];          /* first few service UUIDs */
} bt_devinfo;

int bt_device_info(bt_ctx *ctx, const char *path, bt_devinfo *out);

/* ---- adapters ---- */
int bt_list_adapters(bt_ctx *ctx, char paths[][BT_PATH_LEN], int max);
int bt_select_adapter(bt_ctx *ctx, const char *path);

/* File descriptor to poll for incoming BlueZ signals. */
int bt_get_fd(bt_ctx *ctx);
/* Drain all pending bus traffic, applying every signal to the model; call
 * after poll() reports the fd readable. Returns messages processed, or -errno.
 * May issue a resync when a signal referred to something not in the model. */
int bt_process(bt_ctx *ctx);
/* True (and cleared) if the model changed since the last check, i.e. the UI
 * has something new to draw. */
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

#include "bt.h"
#include "log.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>

#define BLUEZ_SVC     "org.bluez"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE  "org.bluez.Device1"
#define AGENTMGR_IFACE "org.bluez.AgentManager1"
#define AGENT_IFACE   "org.bluez.Agent1"
#define AGENT_PATH    "/org/blttui/agent"
#define AGENT_CAPS    "KeyboardDisplay"

struct bt_ctx {
    sd_bus *bus;
    char    adapter[BT_PATH_LEN]; /* e.g. "/org/bluez/hci0" */
    int     dirty;                /* set by signal callbacks     */
    bt_agent_cb agent;
    bool    agent_set;
};

/* ---- small helpers for reading typed variants out of an a{sv} dict ---- */

static int read_var_str(sd_bus_message *m, const char **out) {
    int r = sd_bus_message_enter_container(m, 'v', "s");
    if (r < 0) return r;
    r = sd_bus_message_read_basic(m, 's', out);
    if (r < 0) return r;
    return sd_bus_message_exit_container(m);
}

static int read_var_bool(sd_bus_message *m, bool *out) {
    int v = 0;
    int r = sd_bus_message_enter_container(m, 'v', "b");
    if (r < 0) return r;
    r = sd_bus_message_read_basic(m, 'b', &v);
    if (r < 0) return r;
    *out = v ? true : false;
    return sd_bus_message_exit_container(m);
}

static int read_var_int16(sd_bus_message *m, short *out) {
    int16_t v = 0;
    int r = sd_bus_message_enter_container(m, 'v', "n");
    if (r < 0) return r;
    r = sd_bus_message_read_basic(m, 'n', &v);
    if (r < 0) return r;
    *out = v;
    return sd_bus_message_exit_container(m);
}

/* Parse an a{sv} property dict for a Device1 object into `d`. */
static int parse_device_props(sd_bus_message *m, bt_device *d) {
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) return r;

    bool have_alias = false;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *key = NULL;
        if ((r = sd_bus_message_read_basic(m, 's', &key)) < 0) return r;

        if (strcmp(key, "Address") == 0) {
            const char *v = NULL;
            if ((r = read_var_str(m, &v)) < 0) return r;
            snprintf(d->address, sizeof d->address, "%s", v);
            if (!have_alias) snprintf(d->name, sizeof d->name, "%s", v);
        } else if (strcmp(key, "Alias") == 0) {
            const char *v = NULL;
            if ((r = read_var_str(m, &v)) < 0) return r;
            snprintf(d->name, sizeof d->name, "%s", v);
            have_alias = true;
        } else if (strcmp(key, "Name") == 0) {
            const char *v = NULL;
            if ((r = read_var_str(m, &v)) < 0) return r;
            if (!have_alias) snprintf(d->name, sizeof d->name, "%s", v);
        } else if (strcmp(key, "Paired") == 0) {
            if ((r = read_var_bool(m, &d->paired)) < 0) return r;
        } else if (strcmp(key, "Connected") == 0) {
            if ((r = read_var_bool(m, &d->connected)) < 0) return r;
        } else if (strcmp(key, "Trusted") == 0) {
            if ((r = read_var_bool(m, &d->trusted)) < 0) return r;
        } else if (strcmp(key, "RSSI") == 0) {
            if ((r = read_var_int16(m, &d->rssi)) < 0) return r;
            d->has_rssi = true;
        } else {
            if ((r = sd_bus_message_skip(m, "v")) < 0) return r;
        }

        if ((r = sd_bus_message_exit_container(m)) < 0) return r; /* sv */
    }
    if (r < 0) return r;
    return sd_bus_message_exit_container(m); /* a{sv} */
}

/* Walk GetManagedObjects. For every object, `want_iface` selects which
 * interface we care about. If `devs` is set we collect Device1 objects;
 * if `adapter_out` is set we record the first Adapter1 object path. */
static int walk_objects(sd_bus *bus,
                        bt_device *devs, size_t max, int *ndev,
                        char *adapter_out, size_t adapter_sz) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int r = sd_bus_call_method(bus, BLUEZ_SVC, "/",
                               "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", &err, &m, "");
    if (r < 0) {
        fprintf(stderr, "GetManagedObjects: %s\n", err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return r;
    }

    if (ndev) *ndev = 0;

    r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}");
    if (r < 0) goto done;

    while ((r = sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}")) > 0) {
        const char *opath = NULL;
        if ((r = sd_bus_message_read_basic(m, 'o', &opath)) < 0) goto done;

        if ((r = sd_bus_message_enter_container(m, 'a', "{sa{sv}}")) < 0) goto done;

        while ((r = sd_bus_message_enter_container(m, 'e', "sa{sv}")) > 0) {
            const char *iface = NULL;
            if ((r = sd_bus_message_read_basic(m, 's', &iface)) < 0) goto done;

            if (devs && ndev && strcmp(iface, DEVICE_IFACE) == 0 && (size_t)*ndev < max) {
                bt_device *d = &devs[*ndev];
                memset(d, 0, sizeof *d);
                snprintf(d->path, sizeof d->path, "%s", opath);
                if ((r = parse_device_props(m, d)) < 0) goto done;
                (*ndev)++;
            } else if (adapter_out && strcmp(iface, ADAPTER_IFACE) == 0) {
                if (adapter_out[0] == '\0')
                    snprintf(adapter_out, adapter_sz, "%s", opath);
                if ((r = sd_bus_message_skip(m, "a{sv}")) < 0) goto done;
            } else {
                if ((r = sd_bus_message_skip(m, "a{sv}")) < 0) goto done;
            }

            if ((r = sd_bus_message_exit_container(m)) < 0) goto done; /* sa{sv} */
        }
        if (r < 0) goto done;
        if ((r = sd_bus_message_exit_container(m)) < 0) goto done; /* a{sa{sv}} */
        if ((r = sd_bus_message_exit_container(m)) < 0) goto done; /* oa{...} */
    }

done:
    sd_bus_message_unref(m);
    sd_bus_error_free(&err);
    return r < 0 ? r : 0;
}

/* ---- public API ---- */

/* Any BlueZ signal we subscribed to: mark the model dirty for a refresh. */
static int on_bt_signal(sd_bus_message *m, void *userdata, sd_bus_error *e) {
    (void)m; (void)e;
    ((bt_ctx *)userdata)->dirty = 1;
    return 0;
}

bt_ctx *bt_open(const char **err) {
    bt_ctx *c = calloc(1, sizeof *c);
    if (!c) { if (err) *err = "out of memory"; return NULL; }

    int r = sd_bus_open_system(&c->bus);
    if (r < 0) {
        if (err) *err = "cannot connect to system D-Bus";
        free(c);
        return NULL;
    }

    r = walk_objects(c->bus, NULL, 0, NULL, c->adapter, sizeof c->adapter);
    if (r < 0 || c->adapter[0] == '\0') {
        if (err) *err = "no Bluetooth adapter found (is bluetoothd running?)";
        bt_close(c);
        return NULL;
    }
    log_msg("bt_open: adapter %s", c->adapter);

    /* Live updates: any of these signals means the device list may have
     * changed. The callback just raises a flag the UI polls via bt_take_dirty. */
    sd_bus_match_signal(c->bus, NULL, BLUEZ_SVC, NULL,
                        "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                        on_bt_signal, c);
    sd_bus_match_signal(c->bus, NULL, BLUEZ_SVC, NULL,
                        "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                        on_bt_signal, c);
    sd_bus_match_signal(c->bus, NULL, BLUEZ_SVC, NULL,
                        "org.freedesktop.DBus.Properties", "PropertiesChanged",
                        on_bt_signal, c);
    return c;
}

void bt_close(bt_ctx *ctx) {
    if (!ctx) return;
    if (ctx->bus) sd_bus_unref(ctx->bus);
    free(ctx);
}

const char *bt_adapter_path(const bt_ctx *ctx) { return ctx->adapter; }

int bt_get_powered(bt_ctx *ctx, bool *out) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int v = 0;
    int r = sd_bus_get_property_trivial(ctx->bus, BLUEZ_SVC, ctx->adapter,
                                        ADAPTER_IFACE, "Powered", &err, 'b', &v);
    sd_bus_error_free(&err);
    if (r < 0) return r;
    *out = v ? true : false;
    return 0;
}

int bt_set_powered(bt_ctx *ctx, bool on) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_set_property(ctx->bus, BLUEZ_SVC, ctx->adapter,
                                ADAPTER_IFACE, "Powered", &err, "b", (int)on);
    log_msg("set Powered=%d -> %d%s%s", on, r,
            r < 0 && err.message ? " " : "", r < 0 && err.message ? err.message : "");
    sd_bus_error_free(&err);
    return r < 0 ? r : 0;
}

int bt_get_discovering(bt_ctx *ctx, bool *out) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int v = 0;
    int r = sd_bus_get_property_trivial(ctx->bus, BLUEZ_SVC, ctx->adapter,
                                        ADAPTER_IFACE, "Discovering", &err, 'b', &v);
    sd_bus_error_free(&err);
    if (r < 0) return r;
    *out = v ? true : false;
    return 0;
}

static int adapter_method(bt_ctx *ctx, const char *method) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(ctx->bus, BLUEZ_SVC, ctx->adapter,
                               ADAPTER_IFACE, method, &err, NULL, "");
    log_msg("adapter.%s -> %d%s%s", method, r,
            r < 0 && err.message ? " " : "", r < 0 && err.message ? err.message : "");
    sd_bus_error_free(&err);
    return r < 0 ? r : 0;
}

int bt_start_discovery(bt_ctx *ctx) { return adapter_method(ctx, "StartDiscovery"); }
int bt_stop_discovery(bt_ctx *ctx)  { return adapter_method(ctx, "StopDiscovery");  }

/* Sort key: connected first, then paired, then the rest. */
static int dev_rank(const bt_device *d) {
    return d->connected ? 0 : (d->paired ? 1 : 2);
}

static int dev_cmp(const void *a, const void *b) {
    const bt_device *x = a, *y = b;
    int rx = dev_rank(x), ry = dev_rank(y);
    if (rx != ry) return rx - ry;

    /* Among the "other" (nearby, unpaired) devices, order by signal strength:
     * strongest first, entries with a known RSSI ahead of those without. */
    if (rx == 2) {
        if (x->has_rssi != y->has_rssi) return x->has_rssi ? -1 : 1;
        if (x->has_rssi && x->rssi != y->rssi) return y->rssi - x->rssi;
    }
    return strcmp(x->name, y->name); /* alphabetical fallback within a group */
}

int bt_list_devices(bt_ctx *ctx, bt_device *out, size_t max) {
    int n = 0;
    int r = walk_objects(ctx->bus, out, max, &n, NULL, 0);
    if (r < 0) return r;
    if (n > 1) qsort(out, (size_t)n, sizeof *out, dev_cmp);
    return n;
}

static int device_method(bt_ctx *ctx, const char *path, const char *method) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(ctx->bus, BLUEZ_SVC, path,
                               DEVICE_IFACE, method, &err, NULL, "");
    log_msg("device.%s %s -> %d%s%s", method, path, r,
            r < 0 && err.message ? " " : "", r < 0 && err.message ? err.message : "");
    sd_bus_error_free(&err);
    return r < 0 ? r : 0;
}

int bt_connect(bt_ctx *ctx, const char *path)    { return device_method(ctx, path, "Connect"); }
int bt_disconnect(bt_ctx *ctx, const char *path) { return device_method(ctx, path, "Disconnect"); }
int bt_pair(bt_ctx *ctx, const char *path)       { return device_method(ctx, path, "Pair"); }

int bt_remove(bt_ctx *ctx, const char *path) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(ctx->bus, BLUEZ_SVC, ctx->adapter,
                               ADAPTER_IFACE, "RemoveDevice", &err, NULL, "o", path);
    log_msg("adapter.RemoveDevice %s -> %d%s%s", path, r,
            r < 0 && err.message ? " " : "", r < 0 && err.message ? err.message : "");
    sd_bus_error_free(&err);
    return r < 0 ? r : 0;
}

int bt_get_fd(bt_ctx *ctx) { return sd_bus_get_fd(ctx->bus); }

int bt_process(bt_ctx *ctx) {
    int r, n = 0;
    /* Drain fully: each call handles at most one message. */
    while ((r = sd_bus_process(ctx->bus, NULL)) > 0)
        n++;
    return r < 0 ? r : n;
}

bool bt_take_dirty(bt_ctx *ctx) {
    bool d = ctx->dirty != 0;
    ctx->dirty = 0;
    return d;
}

/* ---- pairing agent (org.bluez.Agent1) ---- */

/* Resolve a device object path to a human-friendly label (its Alias). */
static void device_label(bt_ctx *ctx, const char *path, char *buf, size_t sz) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    char *alias = NULL;
    int r = sd_bus_get_property_string(ctx->bus, BLUEZ_SVC, path,
                                       DEVICE_IFACE, "Alias", &err, &alias);
    if (r >= 0 && alias && *alias)
        snprintf(buf, sz, "%s", alias);
    else
        snprintf(buf, sz, "%s", path);
    free(alias);
    sd_bus_error_free(&err);
}

/* Standard BlueZ rejection returned when the user declines a prompt. */
static int agent_reject(sd_bus_error *e) {
    return sd_bus_error_set_const(e, "org.bluez.Error.Rejected", "Rejected by user");
}

static int agent_release(sd_bus_message *m, void *ud, sd_bus_error *e) {
    (void)ud; (void)e;
    log_msg("agent: Release");
    return sd_bus_reply_method_return(m, "");
}

static int agent_request_pin(sd_bus_message *m, void *ud, sd_bus_error *e) {
    bt_ctx *ctx = ud;
    const char *path = NULL;
    int r = sd_bus_message_read(m, "o", &path);
    if (r < 0) return r;

    char label[BT_NAME_LEN];
    device_label(ctx, path, label, sizeof label);
    char pin[64] = {0};
    if (ctx->agent_set && ctx->agent.request_pin &&
        ctx->agent.request_pin(ctx->agent.userdata, label, pin, sizeof pin)) {
        log_msg("agent: RequestPinCode %s -> provided", path);
        return sd_bus_reply_method_return(m, "s", pin);
    }
    log_msg("agent: RequestPinCode %s -> rejected", path);
    return agent_reject(e);
}

static int agent_display_pin(sd_bus_message *m, void *ud, sd_bus_error *e) {
    bt_ctx *ctx = ud; (void)e;
    const char *path = NULL, *pincode = NULL;
    int r = sd_bus_message_read(m, "os", &path, &pincode);
    if (r < 0) return r;

    char label[BT_NAME_LEN];
    device_label(ctx, path, label, sizeof label);
    log_msg("agent: DisplayPinCode %s = %s", path, pincode);
    if (ctx->agent_set && ctx->agent.display) {
        char msg[96];
        snprintf(msg, sizeof msg, "Enter PIN on the device: %s", pincode);
        ctx->agent.display(ctx->agent.userdata, label, msg);
    }
    return sd_bus_reply_method_return(m, "");
}

static int agent_request_passkey(sd_bus_message *m, void *ud, sd_bus_error *e) {
    bt_ctx *ctx = ud;
    const char *path = NULL;
    int r = sd_bus_message_read(m, "o", &path);
    if (r < 0) return r;

    char label[BT_NAME_LEN];
    device_label(ctx, path, label, sizeof label);
    unsigned passkey = 0;
    if (ctx->agent_set && ctx->agent.request_passkey &&
        ctx->agent.request_passkey(ctx->agent.userdata, label, &passkey)) {
        log_msg("agent: RequestPasskey %s -> provided", path);
        return sd_bus_reply_method_return(m, "u", (uint32_t)passkey);
    }
    log_msg("agent: RequestPasskey %s -> rejected", path);
    return agent_reject(e);
}

static int agent_display_passkey(sd_bus_message *m, void *ud, sd_bus_error *e) {
    bt_ctx *ctx = ud; (void)e;
    const char *path = NULL;
    uint32_t passkey = 0;
    uint16_t entered = 0;
    int r = sd_bus_message_read(m, "ouq", &path, &passkey, &entered);
    if (r < 0) return r;

    char label[BT_NAME_LEN];
    device_label(ctx, path, label, sizeof label);
    log_msg("agent: DisplayPasskey %s = %06u", path, passkey);
    if (ctx->agent_set && ctx->agent.display) {
        char msg[96];
        snprintf(msg, sizeof msg, "Enter this passkey on the device:\n\n    %06u", passkey);
        ctx->agent.display(ctx->agent.userdata, label, msg);
    }
    return sd_bus_reply_method_return(m, "");
}

static int agent_request_confirmation(sd_bus_message *m, void *ud, sd_bus_error *e) {
    bt_ctx *ctx = ud;
    const char *path = NULL;
    uint32_t passkey = 0;
    int r = sd_bus_message_read(m, "ou", &path, &passkey);
    if (r < 0) return r;

    char label[BT_NAME_LEN];
    device_label(ctx, path, label, sizeof label);
    if (ctx->agent_set && ctx->agent.confirm &&
        ctx->agent.confirm(ctx->agent.userdata, label, (long)passkey)) {
        log_msg("agent: RequestConfirmation %s (%06u) -> accepted", path, passkey);
        return sd_bus_reply_method_return(m, "");
    }
    log_msg("agent: RequestConfirmation %s -> rejected", path);
    return agent_reject(e);
}

static int agent_request_authorization(sd_bus_message *m, void *ud, sd_bus_error *e) {
    bt_ctx *ctx = ud;
    const char *path = NULL;
    int r = sd_bus_message_read(m, "o", &path);
    if (r < 0) return r;

    char label[BT_NAME_LEN];
    device_label(ctx, path, label, sizeof label);
    if (ctx->agent_set && ctx->agent.confirm &&
        ctx->agent.confirm(ctx->agent.userdata, label, -1)) {
        log_msg("agent: RequestAuthorization %s -> accepted", path);
        return sd_bus_reply_method_return(m, "");
    }
    log_msg("agent: RequestAuthorization %s -> rejected", path);
    return agent_reject(e);
}

static int agent_authorize_service(sd_bus_message *m, void *ud, sd_bus_error *e) {
    bt_ctx *ctx = ud;
    const char *path = NULL, *uuid = NULL;
    int r = sd_bus_message_read(m, "os", &path, &uuid);
    if (r < 0) return r;

    char label[BT_NAME_LEN];
    device_label(ctx, path, label, sizeof label);
    if (ctx->agent_set && ctx->agent.confirm &&
        ctx->agent.confirm(ctx->agent.userdata, label, -1)) {
        log_msg("agent: AuthorizeService %s %s -> accepted", path, uuid);
        return sd_bus_reply_method_return(m, "");
    }
    log_msg("agent: AuthorizeService %s -> rejected", path);
    return agent_reject(e);
}

static int agent_cancel(sd_bus_message *m, void *ud, sd_bus_error *e) {
    (void)ud; (void)e;
    log_msg("agent: Cancel");
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release",              "",   "",  agent_release,               SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode",       "o",  "s", agent_request_pin,           SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode",       "os", "",  agent_display_pin,           SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey",       "o",  "u", agent_request_passkey,       SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey",       "ouq","",  agent_display_passkey,       SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation",  "ou", "",  agent_request_confirmation,  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o",  "",  agent_request_authorization, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService",     "os", "",  agent_authorize_service,     SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel",               "",   "",  agent_cancel,                SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};

int bt_register_agent(bt_ctx *ctx, const bt_agent_cb *cb) {
    ctx->agent = *cb;
    ctx->agent_set = true;

    int r = sd_bus_add_object_vtable(ctx->bus, NULL, AGENT_PATH,
                                     AGENT_IFACE, agent_vtable, ctx);
    if (r < 0) {
        log_msg("agent: add_object_vtable failed: %s", strerror(-r));
        return r;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(ctx->bus, BLUEZ_SVC, "/org/bluez", AGENTMGR_IFACE,
                           "RegisterAgent", &err, NULL, "os", AGENT_PATH, AGENT_CAPS);
    if (r < 0) {
        log_msg("agent: RegisterAgent failed: %s",
                err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        return r;
    }
    sd_bus_error_free(&err);

    /* Becoming the default agent may be denied if another one holds it
     * (e.g. a running bluetoothctl). That is non-fatal. */
    err = (sd_bus_error)SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(ctx->bus, BLUEZ_SVC, "/org/bluez", AGENTMGR_IFACE,
                           "RequestDefaultAgent", &err, NULL, "o", AGENT_PATH);
    if (r < 0)
        log_msg("agent: RequestDefaultAgent (non-fatal): %s",
                err.message ? err.message : strerror(-r));
    else
        log_msg("agent: registered as default (%s)", AGENT_CAPS);
    sd_bus_error_free(&err);
    return 0;
}

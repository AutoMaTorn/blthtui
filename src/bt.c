#define _POSIX_C_SOURCE 200809L /* clock_gettime under -std=c11 */

#include "bt.h"
#include "log.h"
#include "strutil.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include <time.h>

#define BLUEZ_SVC     "org.bluez"
#define ADAPTER_IFACE "org.bluez.Adapter1"
#define DEVICE_IFACE  "org.bluez.Device1"
#define AGENTMGR_IFACE "org.bluez.AgentManager1"
#define AGENT_IFACE   "org.bluez.Agent1"
#define AGENT_PATH    "/org/bluetui/agent"
#define AGENT_CAPS    "KeyboardDisplay"

/* BlueZ answers Pair() only once the whole exchange is over, and that includes
 * the time our agent spends waiting for the user in a modal dialog. sd-bus
 * defaults to 25s, which a distracted user blows through easily. */
#define PAIR_TIMEOUT_USEC (180 * 1000000ULL)

/* Signals keep the model exact, so this only has to cover the gap where one is
 * genuinely lost (a bus hiccup, a bluetoothd restart). Once a minute is
 * invisible next to the dozens of updates a second an active scan produces. */
#define RESYNC_MS 60000

struct bt_ctx {
    sd_bus *bus;
    char    adapter[BT_PATH_LEN]; /* e.g. "/org/bluez/hci0", "" once gone */
    int     dirty;                /* model changed; UI should redraw */
    bool    live;                 /* signal subscriptions are in place */

    /* The device model, owned here and updated in place from signals. */
    bt_device devs[BT_MAX_DEVICES];
    int     ndev;
    bool    need_sort;            /* order may have changed, not just values */
    bool    need_resync;          /* a signal referred to something unknown */
    long    last_sync_ms;

    /* Adapter properties, mirrored from PropertiesChanged. */
    bool    powered;
    bool    discovering;

    /* At most one asynchronous device action in flight. */
    sd_bus_slot *op_slot;
    const char *op_label;         /* "Connecting" etc., NULL when idle */
    char    op_err[BT_ERR_LEN];
    bool    op_done;              /* reply arrived, result not yet collected */

    bool    we_started_discovery; /* only stop what we started */

    bt_agent_cb agent;
    bool    agent_set;
    sd_bus_slot *agent_slot;      /* owns the exported Agent1 object */
    bool    agent_registered;     /* BlueZ accepted RegisterAgent */
};

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- model helpers ---- */

static bt_device *find_dev(bt_ctx *c, const char *path) {
    for (int i = 0; i < c->ndev; i++)
        if (strcmp(c->devs[i].path, path) == 0) return &c->devs[i];
    return NULL;
}

static bt_device *add_dev(bt_ctx *c, const char *path) {
    if (c->ndev >= BT_MAX_DEVICES) return NULL;
    bt_device *d = &c->devs[c->ndev++];
    memset(d, 0, sizeof *d);
    snprintf(d->path, sizeof d->path, "%s", path);
    c->need_sort = true;
    return d;
}

static void drop_dev(bt_ctx *c, const char *path) {
    for (int i = 0; i < c->ndev; i++) {
        if (strcmp(c->devs[i].path, path) != 0) continue;
        if (i + 1 < c->ndev)
            memmove(&c->devs[i], &c->devs[i + 1],
                    (size_t)(c->ndev - i - 1) * sizeof c->devs[0]);
        c->ndev--;
        c->need_sort = true;
        return;
    }
}

/* Copy BlueZ's own error text into the caller's buffer, falling back to the
 * errno string. `e` may be an unset sd_bus_error. */
static void set_err(char *err, size_t errsz, const sd_bus_error *e, int r) {
    if (!err || errsz == 0) return;
    if (e && e->message && *e->message)
        snprintf(err, errsz, "%s", e->message);
    else
        snprintf(err, errsz, "%s", strerror(r < 0 ? -r : r));
}

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

/* Parse an a{sv} property dict for a Device1 object into `d`.
 *
 * `partial` marks a PropertiesChanged payload, which carries only the keys
 * that actually changed. There the Address->name fallback must not fire: an
 * update mentioning Address but not Alias would otherwise overwrite a
 * perfectly good name with the MAC. */
static int parse_device_props(sd_bus_message *m, bt_device *d, bool partial) {
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) return r;

    bool have_alias = partial;
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

/* Same, for the two Adapter1 properties the UI shows. */
static int parse_adapter_props(sd_bus_message *m, bt_ctx *c) {
    int r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) return r;

    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *key = NULL;
        if ((r = sd_bus_message_read_basic(m, 's', &key)) < 0) return r;

        if (strcmp(key, "Powered") == 0) {
            if ((r = read_var_bool(m, &c->powered)) < 0) return r;
        } else if (strcmp(key, "Discovering") == 0) {
            if ((r = read_var_bool(m, &c->discovering)) < 0) return r;
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
        /* Log, don't fprintf: in the TUI a stray stderr line lands on top of
         * the newt screen. bt_open() reports the failure through *err. */
        log_msg("GetManagedObjects: %s", err.message ? err.message : strerror(-r));
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
                if ((r = parse_device_props(m, d, false)) < 0) goto done;
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

/* InterfacesAdded carries "oa{sa{sv}}" — the new object with the full property
 * set of each interface it gained. That is exactly the shape parse_device_props
 * reads, so a new device costs no follow-up call: the signal itself is the
 * data. */
static int on_interfaces_added(sd_bus_message *m, void *userdata, sd_bus_error *e) {
    bt_ctx *ctx = userdata;
    (void)e;

    const char *path = NULL;
    if (sd_bus_message_read_basic(m, 'o', &path) < 0 || !path) return 0;
    if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") < 0) return 0;

    while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
        const char *iface = NULL;
        if (sd_bus_message_read_basic(m, 's', &iface) < 0) break;

        if (strcmp(iface, DEVICE_IFACE) == 0) {
            bt_device *d = find_dev(ctx, path);
            if (!d) d = add_dev(ctx, path);
            if (d) {
                parse_device_props(m, d, false);
                ctx->dirty = 1;
            } else {
                sd_bus_message_skip(m, "a{sv}"); /* model full */
            }
        } else if (strcmp(iface, ADAPTER_IFACE) == 0) {
            if (ctx->adapter[0] == '\0') {
                snprintf(ctx->adapter, sizeof ctx->adapter, "%s", path);
                log_msg("adapter appeared: %s", ctx->adapter);
                ctx->need_resync = true; /* pick up its devices and state */
                ctx->dirty = 1;
            }
            sd_bus_message_skip(m, "a{sv}");
        } else {
            sd_bus_message_skip(m, "a{sv}");
        }

        if (sd_bus_message_exit_container(m) < 0) break; /* sa{sv} */
    }
    sd_bus_message_exit_container(m);
    return 0;
}

/* PropertiesChanged carries "sa{sv}as" and, crucially, only the keys that
 * changed — during a scan that is usually a lone RSSI. Applying it in place is
 * what removes the full re-enumeration from the hot path. */
static int on_properties_changed(sd_bus_message *m, void *userdata, sd_bus_error *e) {
    bt_ctx *ctx = userdata;
    (void)e;

    const char *path = sd_bus_message_get_path(m);
    const char *iface = NULL;
    if (!path || sd_bus_message_read_basic(m, 's', &iface) < 0 || !iface) return 0;

    if (strcmp(iface, DEVICE_IFACE) == 0) {
        bt_device *d = find_dev(ctx, path);
        if (!d) {
            /* A device we never saw appear: our model has a gap. */
            ctx->need_resync = true;
            ctx->dirty = 1;
            return 0;
        }
        bool was_connected = d->connected, was_paired = d->paired;
        if (parse_device_props(m, d, true) >= 0) {
            /* Only these two move a row between groups. An RSSI change must
             * not reorder the list — rows would slide under the cursor. */
            if (d->connected != was_connected || d->paired != was_paired)
                ctx->need_sort = true;
            ctx->dirty = 1;
        }
    } else if (strcmp(iface, ADAPTER_IFACE) == 0 &&
               ctx->adapter[0] != '\0' && strcmp(path, ctx->adapter) == 0) {
        if (parse_adapter_props(m, ctx) >= 0)
            ctx->dirty = 1;
    }
    return 0;
}

/* InterfacesRemoved carries "oas": the object that went away and which of its
 * interfaces went with it. We only care about one case — our own adapter being
 * unplugged — because every call we make afterwards would fail with a
 * confusing errno while the UI kept pretending all was well. */
static int on_interfaces_removed(sd_bus_message *m, void *userdata, sd_bus_error *e) {
    bt_ctx *ctx = userdata;
    (void)e;
    ctx->dirty = 1;

    const char *path = NULL;
    if (sd_bus_message_read_basic(m, 'o', &path) < 0 || !path)
        return 0;

    bool is_adapter = ctx->adapter[0] != '\0' && strcmp(path, ctx->adapter) == 0;

    if (sd_bus_message_enter_container(m, 'a', "s") < 0)
        return 0;
    const char *iface = NULL;
    while (sd_bus_message_read_basic(m, 's', &iface) > 0) {
        if (is_adapter && strcmp(iface, ADAPTER_IFACE) == 0) {
            log_msg("adapter %s went away", ctx->adapter);
            ctx->adapter[0] = '\0';
            ctx->ndev = 0;      /* its devices went with it */
            ctx->powered = ctx->discovering = false;
        } else if (strcmp(iface, DEVICE_IFACE) == 0) {
            drop_dev(ctx, path);
        }
    }
    sd_bus_message_exit_container(m);
    return 0;
}

int bt_ensure_adapter(bt_ctx *ctx) {
    if (ctx->adapter[0] != '\0') return 0;

    int r = walk_objects(ctx->bus, NULL, 0, NULL, ctx->adapter, sizeof ctx->adapter);
    if (r < 0 || ctx->adapter[0] == '\0') return -ENODEV;

    log_msg("adapter reacquired: %s", ctx->adapter);
    bt_sync(ctx);
    return 0;
}

bool bt_live_updates(const bt_ctx *ctx) { return ctx->live; }

/* Read the adapter's two properties. Only used by bt_sync — from then on
 * PropertiesChanged keeps them current for free. */
static void sync_adapter_props(bt_ctx *c) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int v = 0;

    if (sd_bus_get_property_trivial(c->bus, BLUEZ_SVC, c->adapter, ADAPTER_IFACE,
                                    "Powered", &err, 'b', &v) >= 0)
        c->powered = v ? true : false;
    sd_bus_error_free(&err);

    err = (sd_bus_error)SD_BUS_ERROR_NULL;
    if (sd_bus_get_property_trivial(c->bus, BLUEZ_SVC, c->adapter, ADAPTER_IFACE,
                                    "Discovering", &err, 'b', &v) >= 0)
        c->discovering = v ? true : false;
    sd_bus_error_free(&err);
}

int bt_sync(bt_ctx *ctx) {
    if (ctx->adapter[0] == '\0') return -ENODEV;

    int n = 0;
    int r = walk_objects(ctx->bus, ctx->devs, BT_MAX_DEVICES, &n, NULL, 0);
    if (r < 0) return r;

    ctx->ndev = n;
    ctx->need_sort = true;
    ctx->need_resync = false;
    ctx->last_sync_ms = now_ms();
    sync_adapter_props(ctx);
    ctx->dirty = 1;
    log_msg("sync: %d devices, powered=%d discovering=%d",
            n, ctx->powered, ctx->discovering);
    return n;
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

    /* Subscribe BEFORE the initial enumeration. The other order leaves a
     * window in which a device appearing between the two is missed entirely,
     * and the model would carry that gap until the next safety resync. */
    int m1 = sd_bus_match_signal(c->bus, NULL, BLUEZ_SVC, NULL,
                                 "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                                 on_interfaces_added, c);
    int m2 = sd_bus_match_signal(c->bus, NULL, BLUEZ_SVC, NULL,
                                 "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved",
                                 on_interfaces_removed, c);
    int m3 = sd_bus_match_signal(c->bus, NULL, BLUEZ_SVC, NULL,
                                 "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                 on_properties_changed, c);
    c->live = (m1 >= 0 && m2 >= 0 && m3 >= 0);
    if (!c->live)
        log_msg("signal subscriptions failed (%d/%d/%d) — falling back to the timer",
                m1, m2, m3);

    r = walk_objects(c->bus, NULL, 0, NULL, c->adapter, sizeof c->adapter);
    if (r < 0 || c->adapter[0] == '\0') {
        if (err) *err = "no Bluetooth adapter found (is bluetoothd running?)";
        bt_close(c);
        return NULL;
    }
    log_msg("bt_open: adapter %s", c->adapter);

    bt_sync(c);
    return c;
}

void bt_close(bt_ctx *ctx) {
    if (!ctx) return;

    /* Leaving discovery running drains the battery of every machine that ever
     * quits this program. Stop only what we started: BlueZ refcounts discovery
     * per client, and another one's session is not ours to end. */
    if (ctx->we_started_discovery && ctx->adapter[0] != '\0') {
        char err[BT_ERR_LEN] = "";
        bt_stop_discovery(ctx, err, sizeof err);
    }
    if (ctx->op_slot) ctx->op_slot = sd_bus_slot_unref(ctx->op_slot);

    /* Hand the agent back before dropping the bus, so BlueZ sees a clean
     * withdrawal rather than our name simply vanishing. */
    if (ctx->agent_registered) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        int r = sd_bus_call_method(ctx->bus, BLUEZ_SVC, "/org/bluez", AGENTMGR_IFACE,
                                   "UnregisterAgent", &err, NULL, "o", AGENT_PATH);
        log_msg("agent: UnregisterAgent -> %d%s%s", r,
                r < 0 && err.message ? " " : "", r < 0 && err.message ? err.message : "");
        sd_bus_error_free(&err);
        ctx->agent_registered = false;
    }
    if (ctx->agent_slot) sd_bus_slot_unref(ctx->agent_slot);
    if (ctx->bus) sd_bus_unref(ctx->bus);
    free(ctx);
}

const char *bt_adapter_path(const bt_ctx *ctx) { return ctx->adapter; }

bool bt_powered(const bt_ctx *ctx)     { return ctx->powered; }
bool bt_discovering(const bt_ctx *ctx) { return ctx->discovering; }

int bt_set_powered(bt_ctx *ctx, bool on, char *errbuf, size_t errsz) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_set_property(ctx->bus, BLUEZ_SVC, ctx->adapter,
                                ADAPTER_IFACE, "Powered", &err, "b", (int)on);
    log_msg("set Powered=%d -> %d%s%s", on, r,
            r < 0 && err.message ? " " : "", r < 0 && err.message ? err.message : "");
    if (r < 0) set_err(errbuf, errsz, &err, r);
    sd_bus_error_free(&err);
    return r < 0 ? r : 0;
}

static int adapter_method(bt_ctx *ctx, const char *method, char *errbuf, size_t errsz) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(ctx->bus, BLUEZ_SVC, ctx->adapter,
                               ADAPTER_IFACE, method, &err, NULL, "");
    log_msg("adapter.%s -> %d%s%s", method, r,
            r < 0 && err.message ? " " : "", r < 0 && err.message ? err.message : "");
    if (r < 0) set_err(errbuf, errsz, &err, r);
    sd_bus_error_free(&err);
    return r < 0 ? r : 0;
}

int bt_start_discovery(bt_ctx *ctx, char *err, size_t errsz) {
    int r = adapter_method(ctx, "StartDiscovery", err, errsz);
    if (r >= 0) ctx->we_started_discovery = true;
    return r;
}
int bt_stop_discovery(bt_ctx *ctx, char *err, size_t errsz) {
    int r = adapter_method(ctx, "StopDiscovery", err, errsz);
    if (r >= 0) ctx->we_started_discovery = false;
    return r;
}

/* Extra detail the list has no room for. Fetched on demand — a details window
 * is opened rarely, so these round-trips never touch the hot path. */
int bt_device_info(bt_ctx *ctx, const char *path, bt_devinfo *out) {
    memset(out, 0, sizeof *out);

    sd_bus_error err = SD_BUS_ERROR_NULL;
    uint8_t pct = 0;
    /* Battery1 is present only on devices that report a level. */
    if (sd_bus_get_property_trivial(ctx->bus, BLUEZ_SVC, path,
                                    "org.bluez.Battery1", "Percentage",
                                    &err, 'y', &pct) >= 0) {
        out->has_battery = true;
        out->battery = pct;
    }
    sd_bus_error_free(&err);

    err = (sd_bus_error)SD_BUS_ERROR_NULL;
    char *icon = NULL;
    if (sd_bus_get_property_string(ctx->bus, BLUEZ_SVC, path, DEVICE_IFACE,
                                   "Icon", &err, &icon) >= 0 && icon)
        snprintf(out->icon, sizeof out->icon, "%s", icon);
    free(icon);
    sd_bus_error_free(&err);

    err = (sd_bus_error)SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int r = sd_bus_get_property(ctx->bus, BLUEZ_SVC, path, DEVICE_IFACE,
                                "UUIDs", &err, &m, "as");
    if (r >= 0) {
        if (sd_bus_message_enter_container(m, 'a', "s") >= 0) {
            const char *u = NULL;
            while (out->nuuid < (int)(sizeof out->uuid / sizeof out->uuid[0]) &&
                   sd_bus_message_read_basic(m, 's', &u) > 0)
                snprintf(out->uuid[out->nuuid++], sizeof out->uuid[0], "%s", u);
            /* A device may advertise more UUIDs than we keep. Drain the rest so
             * exit_container doesn't fail with -EBUSY on a partial read. */
            while (sd_bus_message_read_basic(m, 's', &u) > 0)
                ;
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_unref(m);
    }
    sd_bus_error_free(&err);
    return 0;
}

/* ---- adapters ---- */

int bt_list_adapters(bt_ctx *ctx, char paths[][BT_PATH_LEN], int max) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *m = NULL;
    int n = 0;

    int r = sd_bus_call_method(ctx->bus, BLUEZ_SVC, "/",
                               "org.freedesktop.DBus.ObjectManager",
                               "GetManagedObjects", &err, &m, "");
    if (r < 0) goto done;
    if ((r = sd_bus_message_enter_container(m, 'a', "{oa{sa{sv}}}")) < 0) goto done;

    while (n < max && sd_bus_message_enter_container(m, 'e', "oa{sa{sv}}") > 0) {
        const char *opath = NULL;
        if (sd_bus_message_read_basic(m, 'o', &opath) < 0) break;
        if (sd_bus_message_enter_container(m, 'a', "{sa{sv}}") < 0) break;

        while (sd_bus_message_enter_container(m, 'e', "sa{sv}") > 0) {
            const char *iface = NULL;
            if (sd_bus_message_read_basic(m, 's', &iface) < 0) break;
            if (strcmp(iface, ADAPTER_IFACE) == 0 && n < max)
                snprintf(paths[n++], BT_PATH_LEN, "%s", opath);
            sd_bus_message_skip(m, "a{sv}");
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
        sd_bus_message_exit_container(m);
    }

done:
    sd_bus_message_unref(m);
    sd_bus_error_free(&err);
    return r < 0 ? r : n;
}

int bt_select_adapter(bt_ctx *ctx, const char *path) {
    snprintf(ctx->adapter, sizeof ctx->adapter, "%s", path);
    ctx->ndev = 0;
    log_msg("adapter selected: %s", path);
    return bt_sync(ctx) < 0 ? -ENODEV : 0;
}

int bt_devices(bt_ctx *ctx, const bt_device **out) {
    /* Re-sort only when the order can have changed. Doing it on every read
     * would undo the point: RSSI updates arrive constantly during a scan and
     * would shuffle rows under the user's cursor. */
    if (ctx->need_sort && ctx->ndev > 1) {
        qsort(ctx->devs, (size_t)ctx->ndev, sizeof ctx->devs[0], dev_cmp);
        ctx->need_sort = false;
    }
    if (out) *out = ctx->devs;
    return ctx->ndev;
}

bool bt_truncated(const bt_ctx *ctx) { return ctx->ndev >= BT_MAX_DEVICES; }

/* ---- asynchronous device actions ---- */

static int on_action_reply(sd_bus_message *m, void *userdata, sd_bus_error *e) {
    bt_ctx *ctx = userdata;
    (void)e;

    const sd_bus_error *rep = sd_bus_message_get_error(m);
    if (rep) {
        set_err(ctx->op_err, sizeof ctx->op_err, rep, sd_bus_message_get_errno(m));
        log_msg("action %s failed: %s", ctx->op_label ? ctx->op_label : "?",
                ctx->op_err);
    } else {
        ctx->op_err[0] = '\0';
        log_msg("action %s ok", ctx->op_label ? ctx->op_label : "?");
    }

    ctx->op_done = true;   /* the slot is released in bt_process */
    ctx->dirty = 1;
    return 0;
}

int bt_action_start(bt_ctx *ctx, const char *path, bt_action act,
                    char *errbuf, size_t errsz) {
    if (ctx->op_label) {
        set_err(errbuf, errsz, NULL, -EBUSY);
        return -EBUSY;
    }

    const char *method, *label;
    uint64_t timeout = 0;
    switch (act) {
    case BT_ACT_CONNECT:    method = "Connect";    label = "Connecting";    break;
    case BT_ACT_DISCONNECT: method = "Disconnect"; label = "Disconnecting"; break;
    default:                method = "Pair";       label = "Pairing";
                            timeout = PAIR_TIMEOUT_USEC;                    break;
    }

    sd_bus_message *req = NULL;
    int r = sd_bus_message_new_method_call(ctx->bus, &req, BLUEZ_SVC, path,
                                           DEVICE_IFACE, method);
    if (r < 0) {
        set_err(errbuf, errsz, NULL, r);
        return r;
    }

    r = sd_bus_call_async(ctx->bus, &ctx->op_slot, req, on_action_reply, ctx, timeout);
    sd_bus_message_unref(req);
    if (r < 0) {
        set_err(errbuf, errsz, NULL, r);
        return r;
    }

    ctx->op_label = label;
    ctx->op_done = false;
    ctx->op_err[0] = '\0';
    log_msg("action %s %s started", label, path);
    return 0;
}

const char *bt_action_active(const bt_ctx *ctx) { return ctx->op_label; }

bool bt_action_result(bt_ctx *ctx, char *label, size_t labelsz,
                      char *err, size_t errsz) {
    if (!ctx->op_done) return false;

    if (label && labelsz) snprintf(label, labelsz, "%s", ctx->op_label ? ctx->op_label : "");
    if (err && errsz)     snprintf(err, errsz, "%s", ctx->op_err);

    ctx->op_done = false;
    ctx->op_label = NULL;
    return true;
}

int bt_set_trusted(bt_ctx *ctx, const char *path, bool on,
                   char *errbuf, size_t errsz) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_set_property(ctx->bus, BLUEZ_SVC, path, DEVICE_IFACE,
                                "Trusted", &err, "b", (int)on);
    log_msg("device.Trusted=%d %s -> %d%s%s", on, path, r,
            r < 0 && err.message ? " " : "", r < 0 && err.message ? err.message : "");
    if (r < 0) set_err(errbuf, errsz, &err, r);
    sd_bus_error_free(&err);
    return r < 0 ? r : 0;
}

int bt_remove(bt_ctx *ctx, const char *path, char *errbuf, size_t errsz) {
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(ctx->bus, BLUEZ_SVC, ctx->adapter,
                               ADAPTER_IFACE, "RemoveDevice", &err, NULL, "o", path);
    log_msg("adapter.RemoveDevice %s -> %d%s%s", path, r,
            r < 0 && err.message ? " " : "", r < 0 && err.message ? err.message : "");
    if (r < 0) set_err(errbuf, errsz, &err, r);
    sd_bus_error_free(&err);
    return r < 0 ? r : 0;
}

int bt_get_fd(bt_ctx *ctx) { return sd_bus_get_fd(ctx->bus); }

int bt_process(bt_ctx *ctx) {
    int r, n = 0;
    /* Drain fully: each call handles at most one message. Every signal is
     * applied to the model by its handler, so this is the whole update path. */
    while ((r = sd_bus_process(ctx->bus, NULL)) > 0)
        n++;

    /* Release a finished action's slot here rather than inside its own
     * callback, where sd-bus still owns it. */
    if (ctx->op_done && ctx->op_slot)
        ctx->op_slot = sd_bus_slot_unref(ctx->op_slot);

    /* Two ways the model can drift: a signal named something we never saw, or
     * a lost message. Both are rare, and a resync is one round-trip. */
    if (ctx->adapter[0] != '\0' &&
        (ctx->need_resync || now_ms() - ctx->last_sync_ms > RESYNC_MS))
        bt_sync(ctx);

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

    int r = sd_bus_add_object_vtable(ctx->bus, &ctx->agent_slot, AGENT_PATH,
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
        /* Take the exported object down with us: an agent BlueZ never accepted
         * must not stay answerable on the bus. */
        ctx->agent_slot = sd_bus_slot_unref(ctx->agent_slot);
        ctx->agent_set = false;
        return r;
    }
    ctx->agent_registered = true;
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

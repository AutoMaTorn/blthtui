#define _XOPEN_SOURCE 700 /* expose wcwidth() under -std=c11 */

#include "ui.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <newt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#define REFRESH_MS    4000 /* safety net; signals do the real work */
#define MIN_REDRAW_MS 100  /* coalesce bursts: at most ~10 redraws a second */
#define NAME_COLS     40   /* column budget for the device name */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* `detail` is BlueZ's own explanation when we have one; it beats strerror on
 * the errno by a mile ("br-connection-profile-unavailable" vs "I/O error"). */
static void show_error(const char *action, int err, const char *detail) {
    const char *text = (detail && *detail) ? detail : strerror(-err);
    log_msg("UI error: %s failed: %s", action, text);
    newtWinMessage("Error", "OK", "%s failed:\n\n%s", action, text);
}

/* ---- pairing agent callbacks (invoked by bt.c during pairing) ---- */

static bool ui_confirm(void *ud, const char *dev, long passkey) {
    (void)ud;
    int r;
    if (passkey < 0)
        r = newtWinChoice("Pairing", "Accept", "Reject",
                          "Authorize pairing with\n%s ?", dev);
    else
        r = newtWinChoice("Pairing", "Accept", "Reject",
                          "Confirm this passkey matches\n%s:\n\n    %06ld",
                          dev, passkey);
    return r == 1;
}

static bool ui_request_passkey(void *ud, const char *dev, unsigned *out) {
    (void)ud;
    char *val = NULL;
    struct newtWinEntry items[] = {
        { (char *)"Passkey:", &val, 0 },
        { NULL, NULL, 0 },
    };
    char text[96];
    snprintf(text, sizeof text, "Enter the passkey shown on %s:", dev);
    int r = newtWinEntries((char *)"Pairing", text, 50, 5, 5, 20,
                           items, (char *)"OK", (char *)"Cancel", NULL);
    bool ok = false;
    if (r == 1 && val && *val) {
        *out = (unsigned)strtoul(val, NULL, 10);
        ok = true;
    }
    free(val);
    return ok;
}

static bool ui_request_pin(void *ud, const char *dev, char *buf, size_t sz) {
    (void)ud;
    char *val = NULL;
    struct newtWinEntry items[] = {
        { (char *)"PIN:", &val, 0 },
        { NULL, NULL, 0 },
    };
    char text[96];
    snprintf(text, sizeof text, "Enter the PIN for %s:", dev);
    int r = newtWinEntries((char *)"Pairing", text, 50, 5, 5, 20,
                           items, (char *)"OK", (char *)"Cancel", NULL);
    bool ok = false;
    if (r == 1 && val && *val) {
        snprintf(buf, sz, "%s", val);
        ok = true;
    }
    free(val);
    return ok;
}

static void ui_display(void *ud, const char *dev, const char *what) {
    (void)ud;
    newtWinMessage((char *)"Pairing", (char *)"OK", "%s\n\n%s", dev, what);
}

/* Copy at most `cols` terminal columns of UTF-8 text into buf, then pad with
 * spaces to exactly that width.
 *
 * printf's "%-40.40s" cannot do this: its precision counts BYTES. A device
 * named in Cyrillic — or with the emoji vendors love — gets cut mid-character,
 * the terminal prints replacement garbage, and every column after it slides.
 * Non-printing characters become '?' rather than reaching the terminal, since
 * device names are attacker-supplied and an embedded escape sequence would
 * otherwise be interpreted. */
static void pad_utf8(char *buf, size_t bufsz, const char *src, int cols) {
    size_t out = 0;
    int used = 0;
    mbstate_t st;
    memset(&st, 0, sizeof st);

    while (*src && used < cols) {
        wchar_t wc = 0;
        size_t n = mbrtowc(&wc, src, MB_CUR_MAX, &st);
        int w;

        if (n == (size_t)-1 || n == (size_t)-2) { /* invalid or truncated */
            memset(&st, 0, sizeof st);
            n = 1;
            w = 1;
            wc = L'?';
        } else if (n == 0) {
            break;                                /* embedded NUL */
        } else {
            w = wcwidth(wc);
            if (w < 0) { w = 1; wc = L'?'; }      /* control character */
        }

        if (used + w > cols) break;               /* would overrun the budget */

        if (wc == L'?' ) {
            if (out + 1 >= bufsz) break;
            buf[out++] = '?';
        } else {
            if (out + n >= bufsz) break;
            memcpy(buf + out, src, n);
            out += n;
        }
        src  += n;
        used += w;
    }

    while (used < cols && out + 1 < bufsz) { buf[out++] = ' '; used++; }
    buf[out] = '\0';
}

/* Build a one-line listbox label for a device: name only (BlueZ already
 * falls back to the address when a device has no friendly name). */
static void format_device(const bt_device *d, char *buf, size_t sz) {
    /* Plain ASCII markers so every terminal renders them: nmtui-style. */
    const char *mark = d->connected ? "* " : (d->paired ? "+ " : "  ");
    char name[BT_NAME_LEN + NAME_COLS];
    pad_utf8(name, sizeof name, d->name, NAME_COLS);
    char rssi[16] = "";
    if (d->has_rssi && !d->connected)
        snprintf(rssi, sizeof rssi, "   %ddBm", d->rssi);
    snprintf(buf, sz, "%s%s%s", mark, name, rssi);
}

/* What the list is currently showing, narrowed from the model. */
typedef struct {
    char filter[32];   /* substring match on the name, "" for all */
    bool paired_only;
} ui_view;

/* Case-insensitive substring match, ASCII folding only — enough for the
 * "type a few letters to find your headset" case this serves. */
static bool name_matches(const char *name, const char *needle) {
    if (!*needle) return true;
    size_t nl = strlen(needle);
    for (const char *p = name; *p; p++) {
        size_t i = 0;
        while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return true;
    }
    return false;
}

/* Redraw from the model. No bus traffic at all in the common case: the device
 * list and the adapter state are already current, kept there by the signal
 * handlers in bt.c.
 *
 * `devs` is the UI's own snapshot of what is on screen — the filtered subset,
 * in display order. Copying rather than holding a pointer into the model keeps
 * the listbox rows and the array the selection indexes into consistent:
 * bt_process() may reorder or drop model entries between two redraws. */
static int refresh(bt_ctx *ctx, newtComponent label, newtComponent list,
                   bt_device *devs, int keep_sel, const ui_view *view) {
    /* The adapter can vanish under us — an unplugged dongle, rfkill, a
     * bluetoothd restart. Say so instead of showing a stale list while every
     * call quietly fails. */
    if (bt_ensure_adapter(ctx) < 0) {
        newtLabelSetText(label, "Bluetooth: no adapter        ");
        newtListboxClear(list);
        newtListboxAppendEntry(list, "  (adapter disconnected)", NULL);
        newtListboxSetCurrent(list, 0);
        return 0;
    }

    const bt_device *model = NULL;
    int total = bt_devices(ctx, &model);
    if (total < 0) total = 0;

    int n = 0;
    for (int i = 0; i < total && n < BT_MAX_DEVICES; i++) {
        if (view->paired_only && !model[i].paired) continue;
        if (!name_matches(model[i].name, view->filter)) continue;
        devs[n++] = model[i];
    }

    char status[120];
    const char *busy = bt_action_active(ctx);
    if (busy) {
        snprintf(status, sizeof status, "%s...", busy);
    } else {
        char extra[48] = "";
        if (view->paired_only || view->filter[0])
            snprintf(extra, sizeof extra, "   [%s%s%s]",
                     view->paired_only ? "paired" : "",
                     (view->paired_only && view->filter[0]) ? " " : "",
                     view->filter);
        snprintf(status, sizeof status, "Bluetooth: %-3s   Scanning: %-3s%s%s",
                 bt_powered(ctx) ? "on" : "off",
                 bt_discovering(ctx) ? "yes" : "no",
                 bt_live_updates(ctx) ? "" : "   [polling]", extra);
    }
    newtLabelSetText(label, status);

    newtListboxClear(list);
    if (n == 0) {
        newtListboxAppendEntry(list,
            (view->filter[0] || view->paired_only) ? "  (nothing matches the filter)"
                                                   : "  (no devices — press <Scan>)",
            NULL);
    } else {
        for (int i = 0; i < n; i++) {
            char line[256];
            format_device(&devs[i], line, sizeof line);
            newtListboxAppendEntry(list, line, (void *)(intptr_t)i);
        }
        /* -1 as the row's data: selecting the notice must not act on devs[0],
         * which is what a NULL payload would decode to. */
        if (bt_truncated(ctx))
            newtListboxAppendEntry(list, "  ... list full, some devices hidden",
                                   (void *)(intptr_t)-1);
    }
    if (keep_sel >= n) keep_sel = n > 0 ? n - 1 : 0;
    if (keep_sel < 0) keep_sel = 0;
    newtListboxSetCurrent(list, keep_sel);
    return n;
}

/* Details BlueZ knows but the one-line list has no room for. */
static void show_details(bt_ctx *ctx, const bt_device *d) {
    bt_devinfo info;
    bt_device_info(ctx, d->path, &info);

    char body[768];
    int k = snprintf(body, sizeof body,
                     "%s\n\nAddress:    %s\nStatus:     %s, %s, %s\n",
                     d->name, d->address,
                     d->connected ? "connected" : "not connected",
                     d->paired ? "paired" : "not paired",
                     d->trusted ? "trusted" : "not trusted");

    if (d->has_rssi && k < (int)sizeof body)
        k += snprintf(body + k, sizeof body - k, "Signal:     %d dBm\n", d->rssi);
    if (info.has_battery && k < (int)sizeof body)
        k += snprintf(body + k, sizeof body - k, "Battery:    %d%%\n", info.battery);
    if (info.icon[0] && k < (int)sizeof body)
        k += snprintf(body + k, sizeof body - k, "Type:       %s\n", info.icon);

    if (info.nuuid && k < (int)sizeof body) {
        k += snprintf(body + k, sizeof body - k, "\nServices:\n");
        for (int i = 0; i < info.nuuid && k < (int)sizeof body; i++)
            k += snprintf(body + k, sizeof body - k, "  %s\n", info.uuid[i]);
    }

    newtWinMessage("Device", "OK", "%s", body);
}

/* Offered only when there is more than one adapter — the usual single-adapter
 * machine must not be asked a question it has no choice about. */
static void pick_adapter(bt_ctx *ctx) {
    char paths[8][BT_PATH_LEN];
    int n = bt_list_adapters(ctx, paths, 8);
    if (n <= 1) return;

    char *items[9];
    for (int i = 0; i < n; i++) items[i] = paths[i];
    items[n] = NULL;

    int sel = 0;
    if (newtWinMenu((char *)"Adapter", (char *)"Which adapter?",
                    50, 5, 5, 6, items, &sel, (char *)"OK", (char *)"Cancel", NULL) == 1)
        bt_select_adapter(ctx, paths[sel]);
}

int ui_run(bt_ctx *ctx) {
    newtInit();
    newtCls();
    newtPushHelpLine(" Enter: connect  p: pair  t: trust  d: details  r: remove"
                     "  s: scan  /: filter  q: quit");
    newtDrawRootText(0, 0, "bluetui — Bluetooth manager");

    pick_adapter(ctx);

    /* Register the pairing agent now that newt can draw the prompts. */
    bt_agent_cb agent = {
        .userdata        = NULL,
        .confirm         = ui_confirm,
        .request_passkey = ui_request_passkey,
        .request_pin     = ui_request_pin,
        .display         = ui_display,
    };
    bt_register_agent(ctx, &agent);

    newtCenteredWindow(60, 18, "Bluetooth");

    newtComponent label = newtLabel(2, 0, "Bluetooth: ...   Scanning: ...");
    newtComponent list  = newtListbox(2, 2, 10,
                                      NEWT_FLAG_SCROLL | NEWT_FLAG_RETURNEXIT);
    newtListboxSetWidth(list, 56);

    /* Compact <Label> buttons — minimalist, static labels. */
    newtComponent b_power  = newtCompactButton(2,  14, "Power");
    newtComponent b_scan   = newtCompactButton(12, 14, "Scan");
    newtComponent b_pair   = newtCompactButton(22, 14, "Pair");
    newtComponent b_remove = newtCompactButton(31, 14, "Remove");
    newtComponent b_quit   = newtCompactButton(43, 14, "Quit");

    newtComponent form = newtForm(NULL, NULL, 0);
    newtFormAddComponents(form, label, list,
                          b_power, b_scan, b_pair, b_remove, b_quit, NULL);
    newtFormSetTimer(form, REFRESH_MS);
    newtFormAddHotKey(form, NEWT_KEY_ESCAPE);
    newtFormAddHotKey(form, NEWT_KEY_F10);
    /* Single-key actions: reaching a button through Tab is not what anyone
     * expects from an nmtui-style list. */
    static const int hotkeys[] = { 'q', 's', 'p', 't', 'd', 'r', 'o', '/' };
    for (size_t i = 0; i < sizeof hotkeys / sizeof hotkeys[0]; i++)
        newtFormAddHotKey(form, hotkeys[i]);

    /* Wake the form whenever BlueZ has something to say (device added/removed,
     * property changed, or an agent request), for live updates. */
    int busfd = bt_get_fd(ctx);
    if (busfd >= 0)
        newtFormWatchFd(form, busfd, NEWT_FD_READ);

    static bt_device devs[BT_MAX_DEVICES]; /* static: too big for the stack */
    ui_view view = { .filter = "", .paired_only = false };
    int selected = 0;
    int n = refresh(ctx, label, list, devs, selected, &view);
    long last_draw = now_ms();
    int  pending = 0; /* model changed, redraw owed */

    int running = 1;
    while (running) {
        struct newtExitStruct es;
        newtFormRun(form, &es);

        /* Drain the bus first: applies every signal to the model and runs any
         * agent method calls. Safe here — no form is running mid-dispatch. */
        bt_process(ctx);
        if (bt_take_dirty(ctx)) pending = 1;

        /* An asynchronous action finished: report it once, then let the model
         * updates that follow speak for themselves. */
        char done[32] = "", doneerr[BT_ERR_LEN] = "";
        if (bt_action_result(ctx, done, sizeof done, doneerr, sizeof doneerr)) {
            pending = 1;
            if (doneerr[0]) show_error(done, -EIO, doneerr);
        }

        int cur = (int)(intptr_t)newtListboxGetCurrent(list);
        /* Only real device rows carry a non-negative payload. The "list full"
         * notice carries -1; letting that overwrite `selected` would clamp to 0
         * on the next redraw and yank the cursor to the top of the list. */
        if (cur >= 0) selected = cur;
        const bt_device *sel = (n > 0 && cur >= 0 && cur < n) ? &devs[cur] : NULL;
        char bterr[BT_ERR_LEN] = "";
        int r = 0;

        if (es.reason == NEWT_EXIT_HOTKEY) {
            int key = es.u.key;
            pending = 1;
            if (key == NEWT_KEY_ESCAPE || key == NEWT_KEY_F10 || key == 'q') {
                running = 0;
            } else if (key == 's') {
                r = bt_discovering(ctx) ? bt_stop_discovery(ctx, bterr, sizeof bterr)
                                        : bt_start_discovery(ctx, bterr, sizeof bterr);
                if (r < 0) show_error("Discovery", r, bterr);
            } else if (key == 'o') {
                view.paired_only = !view.paired_only;
                selected = 0;
            } else if (key == '/') {
                char *val = NULL;
                struct newtWinEntry items[] = {
                    { (char *)"Name:", &val, 0 }, { NULL, NULL, 0 },
                };
                if (newtWinEntries((char *)"Filter", (char *)"Show devices matching:",
                                   50, 5, 5, 20, items, (char *)"OK",
                                   (char *)"Clear", NULL) == 1 && val)
                    snprintf(view.filter, sizeof view.filter, "%s", val);
                else
                    view.filter[0] = '\0';
                free(val);
                selected = 0;
            } else if (sel) {
                if (key == 'p') {
                    if ((r = bt_action_start(ctx, sel->path, BT_ACT_PAIR,
                                             bterr, sizeof bterr)) < 0)
                        show_error("Pair", r, bterr);
                } else if (key == 't') {
                    if ((r = bt_set_trusted(ctx, sel->path, !sel->trusted,
                                            bterr, sizeof bterr)) < 0)
                        show_error("Trust", r, bterr);
                } else if (key == 'd') {
                    show_details(ctx, sel);
                } else if (key == 'r') {
                    if (newtWinChoice("Remove device", "Remove", "Cancel",
                                      "Remove %s (%s)?", sel->name, sel->address) == 1 &&
                        (r = bt_remove(ctx, sel->path, bterr, sizeof bterr)) < 0)
                        show_error("Remove", r, bterr);
                }
            }
        } else if (es.reason == NEWT_EXIT_COMPONENT) {
            newtComponent c = es.u.co;
            pending = 1; /* whatever the user did, show the result */
            if (c == b_quit) {
                running = 0;
            } else if (c == b_power) {
                if ((r = bt_set_powered(ctx, !bt_powered(ctx), bterr, sizeof bterr)) < 0)
                    show_error("Toggle power", r, bterr);
            } else if (c == b_scan) {
                r = bt_discovering(ctx) ? bt_stop_discovery(ctx, bterr, sizeof bterr)
                                        : bt_start_discovery(ctx, bterr, sizeof bterr);
                if (r < 0) show_error("Discovery", r, bterr);
            } else if (c == b_pair && sel) {
                if ((r = bt_action_start(ctx, sel->path, BT_ACT_PAIR,
                                         bterr, sizeof bterr)) < 0)
                    show_error("Pair", r, bterr);
            } else if (c == b_remove && sel) {
                if (newtWinChoice("Remove device", "Remove", "Cancel",
                                  "Remove %s (%s)?", sel->name, sel->address) == 1) {
                    if ((r = bt_remove(ctx, sel->path, bterr, sizeof bterr)) < 0)
                        show_error("Remove", r, bterr);
                }
            } else if (c == list && sel) {
                r = bt_action_start(ctx, sel->path,
                                    sel->connected ? BT_ACT_DISCONNECT : BT_ACT_CONNECT,
                                    bterr, sizeof bterr);
                if (r < 0) show_error(sel->connected ? "Disconnect" : "Connect", r, bterr);
            }
        }
        if (!running) break;

        /* Coalesce. An active scan produces dozens of RSSI updates a second;
         * redrawing on each one is pure waste and makes the list flicker. Draw
         * at most every MIN_REDRAW_MS, and when a redraw is owed but too
         * early, ask the form to wake us exactly when the window opens instead
         * of waiting out the safety timer. */
        long t = now_ms();
        if (pending && t - last_draw >= MIN_REDRAW_MS) {
            n = refresh(ctx, label, list, devs, selected, &view);
            last_draw = t;
            pending = 0;
            newtFormSetTimer(form, REFRESH_MS);
        } else if (pending) {
            newtFormSetTimer(form, (int)(MIN_REDRAW_MS - (t - last_draw)));
        }
    }

    newtFormDestroy(form);
    newtPopWindow();
    newtPopHelpLine();
    newtFinished();
    return 0;
}

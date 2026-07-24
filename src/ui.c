#include "ui.h"
#include "log.h"

#include <newt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEVICES 128
#define REFRESH_MS  4000   /* fallback poll; live signals do the real work */

static void show_error(const char *action, int err) {
    log_msg("UI error: %s failed: %s", action, strerror(-err));
    newtWinMessage("Error", "OK", "%s failed: %s", action, strerror(-err));
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

/* Build a one-line listbox label for a device: name only (BlueZ already
 * falls back to the address when a device has no friendly name). */
static void format_device(const bt_device *d, char *buf, size_t sz) {
    /* Plain ASCII markers so every terminal renders them: nmtui-style. */
    const char *mark = d->connected ? "* " : (d->paired ? "+ " : "  ");
    char rssi[16] = "";
    if (d->has_rssi && !d->connected)
        snprintf(rssi, sizeof rssi, "   %ddBm", d->rssi);
    snprintf(buf, sz, "%s%-40.40s%s", mark, d->name, rssi);
}

/* Refresh the status line and device list in place, preserving the cursor.
 * Returns the number of devices now shown. */
static int refresh(bt_ctx *ctx, newtComponent label, newtComponent list,
                   bt_device *devs, int keep_sel) {
    int n = bt_list_devices(ctx, devs, MAX_DEVICES);
    if (n < 0) n = 0;

    bool powered = false, discovering = false;
    bt_get_powered(ctx, &powered);
    bt_get_discovering(ctx, &discovering);

    char status[80];
    snprintf(status, sizeof status, "Bluetooth: %-3s   Scanning: %-3s",
             powered ? "on" : "off", discovering ? "yes" : "no");
    newtLabelSetText(label, status);

    newtListboxClear(list);
    if (n == 0) {
        newtListboxAppendEntry(list, "  (no devices — press <Scan>)", NULL);
    } else {
        for (int i = 0; i < n; i++) {
            char line[128];
            format_device(&devs[i], line, sizeof line);
            newtListboxAppendEntry(list, line, (void *)(intptr_t)i);
        }
    }
    if (keep_sel >= n) keep_sel = n > 0 ? n - 1 : 0;
    if (keep_sel < 0) keep_sel = 0;
    newtListboxSetCurrent(list, keep_sel);
    return n;
}

int ui_run(bt_ctx *ctx) {
    newtInit();
    newtCls();
    newtPushHelpLine(" Enter: connect/disconnect   Tab: move   F10/Esc/q: quit");
    newtDrawRootText(0, 0, "blttui — Bluetooth manager");

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
    newtFormAddHotKey(form, 'q');

    /* Wake the form whenever BlueZ has something to say (device added/removed,
     * property changed, or an agent request), for live updates. */
    int busfd = bt_get_fd(ctx);
    if (busfd >= 0)
        newtFormWatchFd(form, busfd, NEWT_FD_READ);

    bt_device devs[MAX_DEVICES];
    int selected = 0;
    int n = refresh(ctx, label, list, devs, selected);

    int running = 1;
    while (running) {
        struct newtExitStruct es;
        newtFormRun(form, &es);

        /* Drain the bus first: dispatches signals (dirty flag) and any agent
         * method calls. Safe here — no form is running mid-dispatch. */
        bt_process(ctx);

        int cur = (int)(intptr_t)newtListboxGetCurrent(list);
        selected = cur;
        const bt_device *sel = (n > 0 && cur >= 0 && cur < n) ? &devs[cur] : NULL;

        if (es.reason == NEWT_EXIT_HOTKEY) {
            if (es.u.key == NEWT_KEY_ESCAPE || es.u.key == NEWT_KEY_F10 ||
                es.u.key == 'q')
                running = 0;
        } else if (es.reason == NEWT_EXIT_COMPONENT) {
            newtComponent c = es.u.co;
            int r = 0;
            if (c == b_quit) {
                running = 0;
            } else if (c == b_power) {
                bool p = false;
                bt_get_powered(ctx, &p);
                if ((r = bt_set_powered(ctx, !p)) < 0) show_error("Toggle power", r);
            } else if (c == b_scan) {
                bool d = false;
                bt_get_discovering(ctx, &d);
                r = d ? bt_stop_discovery(ctx) : bt_start_discovery(ctx);
                if (r < 0) show_error("Discovery", r);
            } else if (c == b_pair && sel) {
                if ((r = bt_pair(ctx, sel->path)) < 0) show_error("Pair", r);
            } else if (c == b_remove && sel) {
                if (newtWinChoice("Remove device", "Remove", "Cancel",
                                  "Remove %s (%s)?", sel->name, sel->address) == 1) {
                    if ((r = bt_remove(ctx, sel->path)) < 0) show_error("Remove", r);
                }
            } else if (c == list && sel) {
                r = sel->connected ? bt_disconnect(ctx, sel->path)
                                   : bt_connect(ctx, sel->path);
                if (r < 0) show_error(sel->connected ? "Disconnect" : "Connect", r);
            }
        }
        /* Timer, actions and hotkeys all fall through to an in-place refresh
         * that keeps the form (and thus the focused component) intact. */
        if (running)
            n = refresh(ctx, label, list, devs, selected);
    }

    newtFormDestroy(form);
    newtPopWindow();
    newtPopHelpLine();
    newtFinished();
    return 0;
}

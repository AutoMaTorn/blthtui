/* Headless smoke test / debug harness for the BlueZ layer — no UI.
 *
 * Exercises bt.c directly so it can be run under gdb or ASan without newt
 * fighting for the terminal. Honours BLUETUI_LOG just like the real binary.
 *
 * Build:  make test   (produces ./bttest)
 * Run:    ./bttest
 *         BLUETUI_LOG=/tmp/bluetui.log ./bttest
 */
#include <stdio.h>

#include "bt.h"
#include "log.h"

int main(void) {
    log_init();

    const char *err = NULL;
    bt_ctx *c = bt_open(&err);
    if (!c) {
        fprintf(stderr, "bt_open failed: %s\n", err);
        log_close();
        return 1;
    }

    printf("adapter: %s\n", bt_adapter_path(c));
    printf("powered: %s   discovering: %s   live updates: %s\n",
           bt_powered(c) ? "yes" : "no",
           bt_discovering(c) ? "yes" : "no",
           bt_live_updates(c) ? "yes" : "no");

    /* bt_open() has already populated the model; no extra round-trip here. */
    const bt_device *devs = NULL;
    int n = bt_devices(c, &devs);
    printf("devices: %d\n", n);
    for (int i = 0; i < n; i++) {
        printf("  [%c%c] %-17s  %s",
               devs[i].connected ? 'C' : ' ',
               devs[i].paired ? 'P' : ' ',
               devs[i].address, devs[i].name);
        if (devs[i].has_rssi) printf("  (rssi %d)", devs[i].rssi);
        printf("\n");
    }

    bt_close(c);
    log_close();
    return 0;
}

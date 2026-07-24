#include <locale.h>
#include <stdio.h>

#include "bt.h"
#include "log.h"
#include "ui.h"

int main(void) {
    /* Without this, mbrtowc/wcwidth work in the C locale and treat UTF-8
     * device names as bytes — which is exactly what the name column must not
     * do. newt needs it for the same reason. */
    setlocale(LC_ALL, "");

    log_init();

    const char *err = NULL;
    bt_ctx *ctx = bt_open(&err);
    if (!ctx) {
        fprintf(stderr, "blthtui: %s\n", err ? err : "initialisation failed");
        log_msg("bt_open failed: %s", err ? err : "?");
        log_close();
        return 1;
    }

    int rc = ui_run(ctx);

    bt_close(ctx);
    log_close();
    return rc;
}

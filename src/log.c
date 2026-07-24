#define _POSIX_C_SOURCE 200809L /* expose localtime_r under -std=c11 */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static FILE *lf = NULL;

void log_init(void) {
    const char *path = getenv("BLTHTUI_LOG");
    if (!path || !*path) return;
    lf = fopen(path, "a");
    if (!lf) return;
    setvbuf(lf, NULL, _IOLBF, 0); /* line-buffered: tail -f sees it live */
    log_msg("==== blthtui session start ====");
}

void log_msg(const char *fmt, ...) {
    if (!lf) return;

    char ts[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    fprintf(lf, "[%s] ", ts);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(lf, fmt, ap);
    va_end(ap);

    fputc('\n', lf);
}

void log_close(void) {
    if (!lf) return;
    log_msg("==== blthtui session end ====");
    fclose(lf);
    lf = NULL;
}

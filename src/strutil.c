#define _XOPEN_SOURCE 700 /* expose wcwidth() under -std=c11 */

#include "strutil.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

bool parse_passkey(const char *s, unsigned *out) {
    if (!s || !*s) return false;

    unsigned v = 0;
    int ndigits = 0;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
        v = v * 10 + (unsigned)(*p - '0');
        if (++ndigits > 6) return false; /* passkeys are 0..999999 */
    }

    *out = v;
    return true;
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
void pad_utf8(char *buf, size_t bufsz, const char *src, int cols) {
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

/* Case-insensitive substring match, ASCII folding only — enough for the
 * "type a few letters to find your headset" case this serves. */
bool name_matches(const char *name, const char *needle) {
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

/* Sort key: connected first, then paired, then the rest. */
static int dev_rank(const bt_device *d) {
    return d->connected ? 0 : (d->paired ? 1 : 2);
}

int dev_cmp(const void *a, const void *b) {
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

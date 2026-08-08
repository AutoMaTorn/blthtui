#ifndef BLUETUI_STRUTIL_H
#define BLUETUI_STRUTIL_H

#include <stdbool.h>
#include <stddef.h>

#include "device.h"

/* Parse a Bluetooth passkey: exactly 1-6 ASCII digits, value 0..999999.
 * Returns false on anything else (empty, letters, overflow, spaces) — an
 * invalid entry must be treated as a rejected prompt, not as passkey 0. */
bool parse_passkey(const char *s, unsigned *out);

/* Copy at most `cols` terminal columns of UTF-8 text into buf, then pad with
 * spaces to exactly that width. Invalid bytes and control characters become
 * '?'. See the implementation comment for why "%-40.40s" cannot do this. */
void pad_utf8(char *buf, size_t bufsz, const char *src, int cols);

/* Case-insensitive substring match, ASCII folding only. */
bool name_matches(const char *name, const char *needle);

/* Device list ordering: connected first, then paired, then the rest by
 * signal strength (strongest first), alphabetical within a group. */
int dev_cmp(const void *a, const void *b);

#endif /* BLUETUI_STRUTIL_H */

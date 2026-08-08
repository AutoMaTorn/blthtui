/* Unit tests for the pure helpers in strutil.c — no D-Bus, no newt.
 *
 * Build and run:  make check
 *
 * Tiny assert framework: each CHECK prints the failing expression and returns
 * non-zero from main, so `make check` fails loudly instead of silently. */
#include <locale.h>
#include <stdio.h>
#include <string.h>

#include "strutil.h"

static int failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---- parse_passkey ---- */

static void test_parse_passkey(void) {
    unsigned out = 0xdead;

    CHECK(parse_passkey("0", &out) && out == 0);
    CHECK(parse_passkey("123456", &out) && out == 123456);
    CHECK(parse_passkey("000042", &out) && out == 42);

    out = 0xdead;
    CHECK(!parse_passkey("", &out));            /* empty is a cancel, not 0 */
    CHECK(!parse_passkey("abc", &out));         /* letters: was silently 0   */
    CHECK(!parse_passkey("12a", &out));         /* trailing junk             */
    CHECK(!parse_passkey("1234567", &out));     /* passkeys are 6 digits max */
    CHECK(!parse_passkey("-1", &out));          /* no signs                  */
    CHECK(!parse_passkey(" 12", &out));         /* no leading space          */
    CHECK(!parse_passkey("12 ", &out));         /* no trailing space         */
    CHECK(out == 0xdead);                       /* untouched on failure      */
}

/* ---- pad_utf8 ---- */

static void test_pad_utf8(void) {
    char buf[64];

    /* ASCII shorter than the budget is space-padded to exactly cols. */
    pad_utf8(buf, sizeof buf, "abc", 5);
    CHECK(strcmp(buf, "abc  ") == 0);

    /* Longer ASCII is cut at cols. */
    pad_utf8(buf, sizeof buf, "abcdefgh", 4);
    CHECK(strcmp(buf, "abcd") == 0);

    /* Cyrillic: 2 bytes per char, 1 column each — byte-precision would cut a
     * character in half; we must not. "абвгд" = 5 cols, 10 bytes. */
    pad_utf8(buf, sizeof buf, "абвгд", 5);
    CHECK(strcmp(buf, "абвгд") == 0);
    /* Budget 3 cols of a 5-char Cyrillic string: 3 whole chars, no torn byte. */
    pad_utf8(buf, sizeof buf, "абвгд", 3);
    CHECK(strcmp(buf, "абв") == 0);

    /* A wide char (CJK, 2 cols) that would overrun the budget is dropped. */
    pad_utf8(buf, sizeof buf, "a世代", 3);   /* a(1) + 世(2) = 3, 代 won't fit */
    CHECK(strcmp(buf, "a世") == 0);

    /* Invalid UTF-8 bytes become '?', one column each. */
    pad_utf8(buf, sizeof buf, "a\xff""b", 5);
    CHECK(strcmp(buf, "a?b  ") == 0);

    /* Control characters become '?' rather than reaching the terminal. */
    pad_utf8(buf, sizeof buf, "a\x1b[31mb", 7);
    CHECK(buf[0] == 'a' && buf[1] == '?' && strchr(buf, '\x1b') == NULL);

    /* Output is always NUL-terminated even when the buffer is tiny. */
    pad_utf8(buf, 4, "abcdefgh", 8);
    CHECK(strlen(buf) == 3 && buf[3] == '\0');
}

/* ---- name_matches ---- */

static void test_name_matches(void) {
    CHECK(name_matches("WH-1000XM5", ""));
    CHECK(name_matches("WH-1000XM5", "wh-1000"));
    CHECK(name_matches("WH-1000XM5", "1000XM"));
    CHECK(!name_matches("WH-1000XM5", "xm4"));
    CHECK(!name_matches("", "x"));
}

/* ---- dev_cmp ---- */

static bt_device dev(const char *name, bool paired, bool connected,
                     bool has_rssi, short rssi) {
    bt_device d;
    memset(&d, 0, sizeof d);
    snprintf(d.name, sizeof d.name, "%s", name);
    d.paired = paired;
    d.connected = connected;
    d.has_rssi = has_rssi;
    d.rssi = rssi;
    return d;
}

static void test_dev_cmp(void) {
    bt_device con = dev("z-connected", true, true, false, 0);
    bt_device pai = dev("a-paired", true, false, false, 0);
    bt_device oth = dev("m-other", false, false, true, -50);

    /* Groups: connected < paired < other. */
    CHECK(dev_cmp(&con, &pai) < 0);
    CHECK(dev_cmp(&pai, &oth) < 0);
    CHECK(dev_cmp(&oth, &con) > 0);

    /* Within "other": known RSSI ahead of unknown, stronger ahead of weaker. */
    bt_device weak   = dev("b-weak", false, false, true, -90);
    bt_device strong = dev("a-strong", false, false, true, -40);
    bt_device norssi = dev("c-norssi", false, false, false, 0);
    CHECK(dev_cmp(&strong, &weak) < 0);
    CHECK(dev_cmp(&weak, &norssi) < 0);

    /* Same group, no RSSI tie-breaker: alphabetical. */
    bt_device a = dev("alpha", true, false, false, 0);
    bt_device b = dev("beta", true, false, false, 0);
    CHECK(dev_cmp(&a, &b) < 0);
    CHECK(dev_cmp(&b, &a) > 0);
    CHECK(dev_cmp(&a, &a) == 0);
}

int main(void) {
    /* pad_utf8 uses mbrtowc/wcwidth, which need a UTF-8 locale. */
    setlocale(LC_ALL, "");

    test_parse_passkey();
    test_pad_utf8();
    test_name_matches();
    test_dev_cmp();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}

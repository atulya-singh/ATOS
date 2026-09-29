#include "fat_names.h"
#include "../lib/string.h"
#include <atos/abi.h>

/* Pure name-handling logic for the FAT driver: no I/O and no kernel
 * state, so tests/host can compile and test it natively. */

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Characters an 8.3 name may hold (after upper-casing). */
static int short_char_ok(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    return c && strchr("$%'-_@~`!(){}^#&", c);
}

int fat_name_eq(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return *a == *b;
}

/* "FOO     TXT" -> "FOO.TXT" (or "foo.txt" per the NT case flags). */
void fat_name_decode(const char raw[11], uint8_t ntres, char *out) {
    size_t n = 0;
    for (int i = 0; i < 8 && raw[i] != ' '; i++) {
        out[n++] = (ntres & NTRES_LOWER_BASE) ? lower(raw[i]) : raw[i];
    }
    if (raw[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && raw[i] != ' '; i++) {
            out[n++] = (ntres & NTRES_LOWER_EXT) ? lower(raw[i]) : raw[i];
        }
    }
    out[n] = '\0';
    if (out[0] == 0x05) out[0] = (char)0xE5; /* escaped first byte */
}

uint8_t fat_name_checksum(const char name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)name[i]);
    return sum;
}

/* Fills an 11-byte 8.3 name if `name` is exactly representable as one,
 * using the NT flags for an all-lowercase base and/or extension. Mixed
 * case, long parts, or odd characters need a long-name entry instead. */
int fat_name_exact(const char *name, char out[11], uint8_t *ntres) {
    const char *dot = strrchr(name, '.');
    size_t len = strlen(name);
    size_t base_len = dot ? (size_t)(dot - name) : len;
    size_t ext_len = dot ? len - base_len - 1 : 0;
    if (base_len < 1 || base_len > 8 || ext_len > 3 || (dot && ext_len == 0)) return 0;

    memset(out, ' ', 11);
    int lower_seen[2] = {0, 0}, upper_seen[2] = {0, 0};
    for (size_t i = 0; i < len; i++) {
        if (name + i == dot) continue;
        int part = dot && name + i > dot;
        char c = name[i];
        if (c >= 'a' && c <= 'z') lower_seen[part] = 1;
        if (c >= 'A' && c <= 'Z') upper_seen[part] = 1;
        c = upper(c);
        if (!short_char_ok(c)) return 0;
        out[part ? 8 + (size_t)(name + i - dot - 1) : i] = c;
    }
    if ((lower_seen[0] && upper_seen[0]) || (lower_seen[1] && upper_seen[1])) return 0;
    *ntres = (uint8_t)((lower_seen[0] ? NTRES_LOWER_BASE : 0) | (lower_seen[1] ? NTRES_LOWER_EXT : 0));
    return 1;
}

/* The "BASIS~N.EXT" alias every long name also needs. */
void fat_name_alias(const char *name, unsigned n, char out[11]) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    const char *base_end = dot ? dot : name + strlen(name);

    char tail[12];
    size_t tl = 0;
    char digits[10];
    size_t nd = 0;
    do digits[nd++] = (char)('0' + n % 10); while (n /= 10);
    tail[tl++] = '~';
    while (nd) tail[tl++] = digits[--nd];

    size_t bl = 0;
    for (const char *p = name; p < base_end && bl < 8 - tl; p++) {
        if (*p == ' ' || *p == '.') continue;
        char c = upper(*p);
        out[bl++] = short_char_ok(c) ? c : '_';
    }
    if (bl == 0) out[bl++] = '_';
    memcpy(out + bl, tail, tl);

    if (dot) {
        size_t el = 0;
        for (const char *p = dot + 1; *p && el < 3; p++) {
            if (*p == ' ' || *p == '.') continue;
            char c = upper(*p);
            out[8 + el++] = short_char_ok(c) ? c : '_';
        }
    }
}

int fat_name_valid(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len >= ATOS_NAME_MAX) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (name[len - 1] == ' ' || name[len - 1] == '.') return 0; /* Windows can't open these */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7F || strchr("\\/:*?\"<>|", (char)c)) return 0;
    }
    return 1;
}

uint16_t fat_encode_date(unsigned year, unsigned month, unsigned day) {
    unsigned y = year < 1980 ? 0 : year - 1980;
    if (y > 127) y = 127;
    return (uint16_t)((y << 9) | (month << 5) | day);
}

uint16_t fat_encode_time(unsigned hour, unsigned minute, unsigned second) {
    return (uint16_t)((hour << 11) | (minute << 5) | (second / 2));
}

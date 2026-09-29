#include "tar.h"

uint64_t tar_parse_octal(const char *s, size_t n) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && s[i] == ' ') i++;
    for (; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

int tar_checksum_ok(const struct tar_header *h) {
    /* Sum of all header bytes, with the checksum field counted as spaces. */
    const uint8_t *b = (const uint8_t *)h;
    uint64_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++) {
        int in_field = i >= offsetof(struct tar_header, checksum) &&
                       i < offsetof(struct tar_header, checksum) + sizeof(h->checksum);
        sum += in_field ? ' ' : b[i];
    }
    return sum == tar_parse_octal(h->checksum, sizeof(h->checksum));
}

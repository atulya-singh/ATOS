/* Protocol arithmetic with no kernel dependencies, so the host unit
 * tests can exercise it directly (tests/host/test_inet.c). */
#include "inet.h"

uint32_t checksum_add(uint32_t sum, const void *data, size_t len) {
    const uint8_t *p = data;
    while (len > 1) {
        sum += (uint32_t)p[0] << 8 | p[1];
        p += 2;
        len -= 2;
    }
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}

uint16_t checksum_finish(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons((uint16_t)~sum);
}

uint32_t checksum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len) {
    uint32_t sum = checksum_add(0, &src, 4);
    sum = checksum_add(sum, &dst, 4);
    return sum + proto + len;
}

int ip_parse(const char *s, uint32_t *out) {
    uint32_t addr = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return -1;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s++ - '0');
            if (v > 255) return -1;
        }
        addr |= v << (8 * part);
        if (part < 3 && *s++ != '.') return -1;
    }
    if (*s) return -1;
    *out = addr;
    return 0;
}

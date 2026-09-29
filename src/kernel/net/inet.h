#pragma once
#include <stddef.h>
#include <stdint.h>

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

/* The Internet checksum (RFC 1071) of `len` bytes, continuing a running
 * sum; checksum_finish folds and complements it. */
uint32_t checksum_add(uint32_t sum, const void *data, size_t len);
uint16_t checksum_finish(uint32_t sum);
/* The TCP/UDP pseudo-header's contribution. */
uint32_t checksum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);

/* Parses dotted-quad text into a network-order address; 0 on success. */
int ip_parse(const char *s, uint32_t *out);

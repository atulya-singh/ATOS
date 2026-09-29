#include "test.h"
#include "net/inet.h"

TEST(checksum_rfc1071_example) {
    /* The worked example from RFC 1071 section 3: sum 0xddf2. */
    const uint8_t data[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK_EQ_INT(ntohs(checksum_finish(checksum_add(0, data, sizeof(data)))), 0xFFFF & ~0xddf2);
}

TEST(checksum_ipv4_header_verifies) {
    /* A real header (from Wikipedia's IPv4 checksum example), checksum 0xb861. */
    uint8_t h[20] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11,
                     0x00, 0x00, 0xc0, 0xa8, 0x00, 0x01, 0xc0, 0xa8, 0x00, 0xc7};
    uint16_t sum = checksum_finish(checksum_add(0, h, sizeof(h)));
    CHECK_EQ_INT(ntohs(sum), 0xb861);
    memcpy(h + 10, &sum, 2);
    CHECK_EQ_INT(checksum_finish(checksum_add(0, h, sizeof(h))), 0); /* receiver's check */
}

TEST(checksum_odd_length_and_chaining) {
    const uint8_t data[] = {0x12, 0x34, 0x56};
    /* An odd trailing byte is padded with zero on the right. */
    CHECK_EQ_INT(checksum_add(0, data, 3), 0x1234 + 0x5600);
    /* Summing in two even-sized pieces equals summing at once. */
    const uint8_t more[] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK_EQ_INT(checksum_finish(checksum_add(checksum_add(0, more, 4), more + 4, 4)),
                 checksum_finish(checksum_add(0, more, 8)));
}

TEST(checksum_pseudo_header) {
    uint32_t src, dst;
    CHECK_EQ_INT(ip_parse("10.0.2.15", &src), 0);
    CHECK_EQ_INT(ip_parse("10.0.2.2", &dst), 0);
    /* 0x0a00 + 0x020f + 0x0a00 + 0x0202 + proto 6 + length 20 */
    CHECK_EQ_INT(checksum_pseudo(src, dst, 6, 20), 0x0a00 + 0x020f + 0x0a00 + 0x0202 + 6 + 20);
}

TEST(ip_parse_forms) {
    uint32_t a;
    CHECK_EQ_INT(ip_parse("10.0.2.15", &a), 0);
    CHECK_EQ_INT(a, htonl(0x0a00020f));
    CHECK_EQ_INT(ip_parse("255.255.255.255", &a), 0);
    CHECK_EQ_INT(a, 0xFFFFFFFF);
    CHECK_EQ_INT(ip_parse("0.0.0.0", &a), 0);
    CHECK_EQ_INT(a, 0);
    CHECK(ip_parse("256.0.0.1", &a) != 0);
    CHECK(ip_parse("1.2.3", &a) != 0);
    CHECK(ip_parse("1.2.3.4.5", &a) != 0);
    CHECK(ip_parse("1..3.4", &a) != 0);
    CHECK(ip_parse("a.b.c.d", &a) != 0);
    CHECK(ip_parse("", &a) != 0);
    CHECK(ip_parse("1.2.3.4 ", &a) != 0);
}

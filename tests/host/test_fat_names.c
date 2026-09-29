#include "test.h"
#include "fs/fat_names.h"
#include <atos/abi.h>

static void expect_exact(const char *name, const char *raw, uint8_t ntres) {
    char out[12] = {0};
    uint8_t flags = 0xFF;
    int ok = fat_name_exact(name, out, &flags);
    CHECK(ok);
    if (!ok) return;
    CHECK_EQ_STR(out, raw);
    CHECK_EQ_INT(flags, ntres);
}

static void expect_not_exact(const char *name) {
    char out[11];
    uint8_t flags;
    CHECK_EQ_INT(fat_name_exact(name, out, &flags), 0);
}

TEST(fat_exact_short_names) {
    expect_exact("HELLO.TXT", "HELLO   TXT", 0);
    expect_exact("hello.txt", "HELLO   TXT", NTRES_LOWER_BASE | NTRES_LOWER_EXT);
    expect_exact("README", "README     ", 0);
    expect_exact("readme", "README     ", NTRES_LOWER_BASE);
    expect_exact("ABC.txt", "ABC     TXT", NTRES_LOWER_EXT);
    expect_exact("12345678.123", "12345678123", 0);
    expect_exact("a-b_c~1.x", "A-B_C~1 X  ", NTRES_LOWER_BASE | NTRES_LOWER_EXT);

    expect_not_exact("Mixed.txt");     /* mixed case in one part */
    expect_not_exact("toolongname.txt");
    expect_not_exact("x.html");        /* extension too long */
    expect_not_exact("a.b.c");
    expect_not_exact("with space.t");
    expect_not_exact("plus+.txt");
    expect_not_exact(".hidden");
    expect_not_exact("trailing.");
}

static void expect_alias(const char *name, unsigned n, const char *raw) {
    char out[12] = {0};
    fat_name_alias(name, n, out);
    CHECK_EQ_STR(out, raw);
}

TEST(fat_alias_names) {
    expect_alias("Mixed Case Name.txt", 1, "MIXEDC~1TXT");
    expect_alias("Mixed Case Name.txt", 12, "MIXED~12TXT");
    expect_alias("Mixed Case Name.txt", 12345, "MI~12345TXT");
    expect_alias(".hidden", 1, "HIDDEN~1   ");
    expect_alias("a+b.txt", 2, "A_B~2   TXT");
    expect_alias("archive.tar.gz", 1, "ARCHIV~1GZ ");
    expect_alias("...", 1, "_~1        ");
    expect_alias("x.markdown", 3, "X~3     MAR");
}

TEST(fat_decode_short_names) {
    char out[13];
    fat_name_decode("HELLO   TXT", 0, out);
    CHECK_EQ_STR(out, "HELLO.TXT");
    fat_name_decode("HELLO   TXT", NTRES_LOWER_BASE | NTRES_LOWER_EXT, out);
    CHECK_EQ_STR(out, "hello.txt");
    fat_name_decode("ABC     TXT", NTRES_LOWER_EXT, out);
    CHECK_EQ_STR(out, "ABC.txt");
    fat_name_decode("README     ", 0, out);
    CHECK_EQ_STR(out, "README");
    fat_name_decode("\x05" "BC     TXT", 0, out);
    CHECK_EQ_INT((unsigned char)out[0], 0xE5);
}

TEST(fat_decode_inverts_exact) {
    const char *names[] = {"hello.txt", "README", "ABC.txt", "x.y", "NOEXT", "lower"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char raw[11], back[13];
        uint8_t flags;
        CHECK(fat_name_exact(names[i], raw, &flags));
        fat_name_decode(raw, flags, back);
        CHECK_EQ_STR(back, names[i]);
    }
}

TEST(fat_name_checksum_matches_spec) {
    /* The spec's rotate-right-and-add, computed longhand. */
    const char *raw = "MIXEDC~1TXT";
    unsigned sum = 0;
    for (int i = 0; i < 11; i++) sum = (((sum & 1) << 7) | (sum >> 1)) + (unsigned char)raw[i], sum &= 0xFF;
    CHECK_EQ_INT(fat_name_checksum(raw), sum);
    CHECK(fat_name_checksum("MIXEDC~1TXT") != fat_name_checksum("MIXEDC~2TXT"));
}

TEST(fat_name_validity) {
    CHECK(fat_name_valid("Mixed Case Name.txt"));
    CHECK(fat_name_valid(".hidden"));
    CHECK(fat_name_valid("a"));
    CHECK(!fat_name_valid(""));
    CHECK(!fat_name_valid("."));
    CHECK(!fat_name_valid(".."));
    CHECK(!fat_name_valid("trailing."));
    CHECK(!fat_name_valid("trailing "));
    CHECK(!fat_name_valid("a/b"));
    CHECK(!fat_name_valid("a*b"));
    CHECK(!fat_name_valid("a\"b"));
    CHECK(!fat_name_valid("tab\there"));

    char name[ATOS_NAME_MAX + 1];
    memset(name, 'n', ATOS_NAME_MAX - 1);
    name[ATOS_NAME_MAX - 1] = '\0';
    CHECK(fat_name_valid(name));
    name[ATOS_NAME_MAX - 1] = 'n';
    name[ATOS_NAME_MAX] = '\0';
    CHECK(!fat_name_valid(name));
}

TEST(fat_name_case_insensitive_eq) {
    CHECK(fat_name_eq("Hello.TXT", "hello.txt"));
    CHECK(fat_name_eq("", ""));
    CHECK(!fat_name_eq("abc", "abcd"));
    CHECK(!fat_name_eq("abcd", "abc"));
    CHECK(!fat_name_eq("a_b", "a-b"));
}

TEST(fat_date_time_encoding) {
    CHECK_EQ_INT(fat_encode_date(2026, 9, 28), (46 << 9) | (9 << 5) | 28);
    CHECK_EQ_INT(fat_encode_date(1980, 1, 1), (0 << 9) | (1 << 5) | 1);
    CHECK_EQ_INT(fat_encode_date(1970, 1, 1) >> 9, 0);     /* clamped low */
    CHECK_EQ_INT(fat_encode_date(2200, 1, 1) >> 9, 127);   /* clamped high */
    CHECK_EQ_INT(fat_encode_time(23, 59, 59), (23 << 11) | (59 << 5) | 29);
    CHECK_EQ_INT(fat_encode_time(0, 0, 1), 0);             /* 2-second units */
}

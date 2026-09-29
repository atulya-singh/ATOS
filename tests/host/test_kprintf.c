#include "test.h"
#include "dev/console.h"
#include "lib/kprintf.h"
#include <limits.h>
#include <stdint.h>

/* Some cases below feed kprintf odd formats on purpose. */
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wformat-overflow"
#pragma GCC diagnostic ignored "-Wformat-extra-args"

/* kprintf's output sink, captured instead of going to a console. */
struct spinlock console_lock;

static char captured[1024];
static size_t captured_len;

void console_putc(char c) {
    if (captured_len < sizeof(captured) - 1) captured[captured_len++] = c;
    captured[captured_len] = '\0';
}

#define EXPECT_PRINT(want, ...)        \
    do {                               \
        captured_len = 0;              \
        captured[0] = '\0';            \
        kprintf(__VA_ARGS__);          \
        CHECK_EQ_STR(captured, want);  \
    } while (0)

TEST(kprintf_integers) {
    EXPECT_PRINT("0 7 -7", "%d %d %d", 0, 7, -7);
    EXPECT_PRINT("-2147483648", "%d", INT_MIN);
    EXPECT_PRINT("-9223372036854775808", "%ld", LONG_MIN);
    EXPECT_PRINT("18446744073709551615", "%lu", UINT64_MAX);
    EXPECT_PRINT("4294967295", "%u", UINT_MAX);
    EXPECT_PRINT("ff FF 0xff", "%x %X %#x", 255, 255, 255);
    EXPECT_PRINT("0x00000000deadbeef", "%#016lx", 0xdeadbeefUL);
    EXPECT_PRINT("   42", "%5u", 42u);
    EXPECT_PRINT("00042", "%05u", 42u);
    EXPECT_PRINT("12345", "%3u", 12345u); /* width is a minimum */
}

TEST(kprintf_strings_and_misc) {
    EXPECT_PRINT("a hello (null) %", "%c %s %s %%", 'a', "hello", (char *)NULL);
    EXPECT_PRINT("0x0000000000001234", "%p", (void *)0x1234);
    EXPECT_PRINT("plain text", "plain text");
    EXPECT_PRINT("%q", "%q");
    EXPECT_PRINT("end %", "end %");
}

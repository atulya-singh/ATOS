#pragma once
/* A deliberately tiny unit-test harness for the host-side tests: each
 * TEST registers itself at startup, CHECKs record failures without
 * stopping the test, and main() (in main.c) runs everything. */
#include <stdio.h>
#include <string.h>

void test_register(const char *name, void (*fn)(void));
void test_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define TEST(name)                                                        \
    static void name(void);                                               \
    __attribute__((constructor)) static void register_##name(void) {      \
        test_register(#name, name);                                       \
    }                                                                     \
    static void name(void)

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) test_fail(__FILE__, __LINE__, "%s", #cond);                \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                \
    do {                                                                  \
        long long a_ = (long long)(a), b_ = (long long)(b);               \
        if (a_ != b_)                                                     \
            test_fail(__FILE__, __LINE__, "%s == %s (%lld vs %lld)", #a, #b, a_, b_); \
    } while (0)

#define CHECK_EQ_STR(a, b)                                                \
    do {                                                                  \
        const char *a_ = (a), *b_ = (b);                                  \
        if (strcmp(a_, b_) != 0)                                          \
            test_fail(__FILE__, __LINE__, "%s == %s (\"%s\" vs \"%s\")", #a, #b, a_, b_); \
    } while (0)

#define CHECK_EQ_MEM(a, b, n) CHECK(memcmp((a), (b), (n)) == 0)

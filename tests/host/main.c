#include "test.h"
#include <stdarg.h>
#include <stdlib.h>

#define MAX_TESTS 256

static struct {
    const char *name;
    void (*fn)(void);
} tests[MAX_TESTS];
static int test_count, current_failed, total_failed;

void test_register(const char *name, void (*fn)(void)) {
    if (test_count == MAX_TESTS) abort();
    tests[test_count].name = name;
    tests[test_count].fn = fn;
    test_count++;
}

void test_fail(const char *file, int line, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("    %s:%d: CHECK failed: ", file, line);
    vprintf(fmt, args);
    printf("\n");
    va_end(args);
    current_failed = 1;
}

int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : NULL;
    int ran = 0;
    for (int i = 0; i < test_count; i++) {
        if (filter && !strstr(tests[i].name, filter)) continue;
        current_failed = 0;
        tests[i].fn();
        printf("%s  %s\n", current_failed ? "FAIL" : "ok  ", tests[i].name);
        total_failed += current_failed;
        ran++;
    }
    printf("host tests: %d run, %d failed\n", ran, total_failed);
    return total_failed ? 1 : 0;
}

#include "crashtest.h"
#include "lib/cmdline.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "sched/sched.h"

/* noinline + volatile keep each level a real frame for the backtrace. */
static __attribute__((noinline)) void crash_deref(volatile uint64_t *p) {
    *p = 0xDEAD;
}

static __attribute__((noinline)) void crash_level2(void) {
    crash_deref((volatile uint64_t *)0);
}

static __attribute__((noinline)) void crash_level1(void) {
    crash_level2();
}

static volatile int keep_recursing = 1;

static __attribute__((noinline)) uint64_t crash_recurse(uint64_t n) {
    volatile uint8_t pad[256];
    pad[0] = (uint8_t)n;
    return (keep_recursing ? crash_recurse(n + 1) : 0) + pad[0];
}

static __attribute__((noinline)) void crash_assert(int value) {
    KASSERT(value == 42);
}

static void crashtest_task(void *arg) {
    const char *kind = arg;
    kprintf("crashtest: triggering %s\n", kind);
    if (strcmp(kind, "pagefault") == 0) crash_level1();
    else if (strcmp(kind, "stackoverflow") == 0) crash_recurse(0);
    else if (strcmp(kind, "assert") == 0) crash_assert(7);
    kprintf("crashtest: unknown kind '%s'\n", kind);
}

void crashtest_start(void) {
    static char kind[32];
    if (cmdline_get("crashtest", kind, sizeof(kind))) {
        task_create_kernel("crashtest", crashtest_task, kind);
    }
}

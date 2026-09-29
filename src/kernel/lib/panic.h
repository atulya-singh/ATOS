#pragma once
#include <stdint.h>

struct registers;

/* Stops the machine with a report: message, current task, and a
 * symbolized backtrace, on serial and on a red panic screen. */
__attribute__((noreturn, format(printf, 1, 2))) void panic(const char *fmt, ...);

/* The same, for a CPU exception taken in kernel mode: adds the full
 * register dump and starts the backtrace at the faulting instruction. */
__attribute__((noreturn)) void panic_exception(const struct registers *regs, const char *what,
                                               uint64_t cr2);

/* Whether some CPU has started panicking (the others are being stopped). */
int panic_in_progress(void);

/* Prints the call chain starting at `rip`, following saved frame pointers
 * from `rbp`. Safe on a corrupted stack: stops at the first frame that
 * isn't mapped kernel memory or doesn't move up the stack. */
void backtrace_print(uint64_t rip, uint64_t rbp);

#define KASSERT(cond)                                                                  \
    do {                                                                               \
        if (!(cond)) panic("assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); \
    } while (0)

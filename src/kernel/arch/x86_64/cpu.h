#pragma once
#include <stdint.h>

/* Single-CPU critical sections: disabling interrupts is the whole lock.
 * Returns the previous RFLAGS so nested sections restore correctly
 * instead of blindly re-enabling interrupts on the way out. */
static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & (1 << 9)) asm volatile("sti" ::: "memory");
}

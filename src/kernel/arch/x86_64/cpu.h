#pragma once
#include <stdint.h>

/* Single-CPU critical sections: disabling interrupts is the whole lock.
 * Returns the previous RFLAGS so nested sections restore correctly
 * instead of blindly re-enabling interrupts on the way out. */
#ifdef ATOS_HOST_TEST
/* Host unit tests run this code as an ordinary process: no cli/sti. */
static inline uint64_t irq_save(void) { return 0; }
static inline void irq_restore(uint64_t flags) { (void)flags; }
#else
static inline uint64_t irq_save(void) {
    uint64_t flags;
    asm volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void irq_restore(uint64_t flags) {
    if (flags & (1 << 9)) asm volatile("sti" ::: "memory");
}
#endif

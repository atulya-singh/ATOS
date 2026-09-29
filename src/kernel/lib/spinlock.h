#pragma once
#include <stdint.h>
#include "../arch/x86_64/cpu.h"

/* Busy-waiting lock for short critical sections shared between CPUs.
 * Anything also touched from an interrupt handler must be taken with the
 * _irqsave variant: otherwise that handler, landing on the CPU that holds
 * the lock, would spin on it forever. Zero-initialized is unlocked. */
struct spinlock {
    volatile uint32_t locked;
    uint32_t owner; /* holding CPU's index + 1, or 0; catches self-deadlock */
};

#ifdef ATOS_HOST_TEST
/* Host unit tests are single-threaded: locks are no-ops. */
static inline void spin_lock(struct spinlock *l) { (void)l; }
static inline void spin_unlock(struct spinlock *l) { (void)l; }
#else
#include "../arch/x86_64/percpu.h"

__attribute__((noreturn)) void spinlock_recursion(struct spinlock *l);

static inline void spin_lock(struct spinlock *l) {
    uint32_t me = this_cpu()->index + 1;
    if (__atomic_load_n(&l->owner, __ATOMIC_RELAXED) == me) spinlock_recursion(l);
    while (__atomic_exchange_n(&l->locked, 1, __ATOMIC_ACQUIRE)) {
        /* Spin on a plain read, not the exchange, so waiting CPUs don't
         * keep stealing the cache line from the holder. */
        while (__atomic_load_n(&l->locked, __ATOMIC_RELAXED)) __builtin_ia32_pause();
    }
    l->owner = me;
}

static inline void spin_unlock(struct spinlock *l) {
    l->owner = 0;
    __atomic_store_n(&l->locked, 0, __ATOMIC_RELEASE);
}
#endif

static inline uint64_t spin_lock_irqsave(struct spinlock *l) {
    uint64_t flags = irq_save();
    spin_lock(l);
    return flags;
}

static inline void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags) {
    spin_unlock(l);
    irq_restore(flags);
}

#include "timer.h"
#include "../arch/x86_64/percpu.h"

static volatile uint64_t ticks;

uint64_t timer_ticks(void) {
    return __atomic_load_n(&ticks, __ATOMIC_RELAXED);
}

void timer_tick(void) {
    if (this_cpu()->index == 0) __atomic_add_fetch(&ticks, 1, __ATOMIC_RELAXED);
}

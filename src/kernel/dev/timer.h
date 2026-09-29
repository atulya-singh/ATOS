#pragma once
#include <stdint.h>

/* System time in ticks since the timers started. Every CPU's LAPIC timer
 * fires at TIMER_HZ to drive its own scheduling; only the bootstrap CPU's
 * advances this count. */
#define TIMER_HZ 100

uint64_t timer_ticks(void);

/* Called from the timer interrupt. */
void timer_tick(void);

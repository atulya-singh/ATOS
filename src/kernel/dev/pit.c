#include "pit.h"
#include "../lib/io.h"

#define PIT_CHANNEL0 0x40
#define PIT_COMMAND  0x43
#define PIT_BASE_FREQUENCY 1193182u

static volatile uint64_t ticks = 0;

void pit_init(uint32_t frequency_hz) {
    uint32_t divisor = PIT_BASE_FREQUENCY / frequency_hz;
    outb(PIT_COMMAND, 0x36); /* channel 0, lobyte/hibyte, rate generator */
    outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));
}

void pit_tick(void) {
    ticks++;
}

uint64_t pit_get_ticks(void) {
    return ticks;
}

#pragma once
#include <stdint.h>

void pit_init(uint32_t frequency_hz);
void pit_tick(void);
uint64_t pit_get_ticks(void);

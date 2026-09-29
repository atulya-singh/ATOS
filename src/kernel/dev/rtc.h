#pragma once
#include <stdint.h>

struct rtc_time {
    uint16_t year; /* e.g. 2026 */
    uint8_t month, day, hour, minute, second;
    uint8_t century; /* raw century register value (internal) */
};

/* Reads the wall-clock time from the CMOS real-time clock. */
void rtc_read(struct rtc_time *out);

/* CMOS index of the century register (from the ACPI FADT); 0 = none, in
 * which case years are assumed to be 20xx. */
void rtc_set_century_register(uint8_t reg);

#pragma once
#include <stdint.h>

struct rtc_time {
    uint16_t year; /* e.g. 2026 */
    uint8_t month, day, hour, minute, second;
};

/* Reads the wall-clock time from the CMOS real-time clock. */
void rtc_read(struct rtc_time *out);

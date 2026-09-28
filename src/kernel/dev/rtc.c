#include "rtc.h"
#include "../arch/x86_64/cpu.h"
#include "../lib/io.h"

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71
#define NMI_DISABLE 0x80

#define REG_SECONDS  0x00
#define REG_MINUTES  0x02
#define REG_HOURS    0x04
#define REG_DAY      0x07
#define REG_MONTH    0x08
#define REG_YEAR     0x09
#define REG_STATUS_A 0x0A
#define REG_STATUS_B 0x0B

#define STATUS_A_UPDATING 0x80
#define STATUS_B_24H      0x02
#define STATUS_B_BINARY   0x04

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_INDEX, NMI_DISABLE | reg);
    return inb(CMOS_DATA);
}

static uint8_t from_bcd(uint8_t v) { return (uint8_t)((v & 0x0F) + (v >> 4) * 10); }

static void read_raw(struct rtc_time *t) {
    while (cmos_read(REG_STATUS_A) & STATUS_A_UPDATING) {}
    t->second = cmos_read(REG_SECONDS);
    t->minute = cmos_read(REG_MINUTES);
    t->hour = cmos_read(REG_HOURS);
    t->day = cmos_read(REG_DAY);
    t->month = cmos_read(REG_MONTH);
    t->year = cmos_read(REG_YEAR);
}

void rtc_read(struct rtc_time *out) {
    uint64_t flags = irq_save(); /* the index/data port pair is shared state */

    /* The clock can tick over mid-read, so read until two consecutive
     * reads agree. */
    struct rtc_time a, b;
    read_raw(&b);
    do {
        a = b;
        read_raw(&b);
    } while (a.second != b.second || a.minute != b.minute || a.hour != b.hour ||
             a.day != b.day || a.month != b.month || a.year != b.year);

    uint8_t status_b = cmos_read(REG_STATUS_B);
    irq_restore(flags);

    int pm = !(status_b & STATUS_B_24H) && (b.hour & 0x80);
    b.hour &= 0x7F;
    if (!(status_b & STATUS_B_BINARY)) {
        b.second = from_bcd(b.second);
        b.minute = from_bcd(b.minute);
        b.hour = from_bcd(b.hour);
        b.day = from_bcd(b.day);
        b.month = from_bcd(b.month);
        b.year = from_bcd((uint8_t)b.year);
    }
    if (!(status_b & STATUS_B_24H)) b.hour = (uint8_t)(b.hour % 12 + (pm ? 12 : 0));
    b.year = (uint16_t)(2000 + b.year); /* no century register without ACPI */
    *out = b;
}

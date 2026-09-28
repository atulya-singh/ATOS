#include "console.h"
#include "../arch/x86_64/cpu.h"
#include "fbcon.h"
#include "serial.h"

void console_putc(char c) {
    serial_putc(c);
    fbcon_putc(c);
}

void console_write(const char *s, size_t len) {
    /* One write stays contiguous on screen, same guarantee kprintf gives. */
    uint64_t flags = irq_save();
    for (size_t i = 0; i < len; i++) console_putc(s[i]);
    irq_restore(flags);
}

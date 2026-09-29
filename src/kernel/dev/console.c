#include "console.h"
#include "fbcon.h"
#include "serial.h"

struct spinlock console_lock;

void console_putc(char c) {
    serial_putc(c);
    fbcon_putc(c);
}

void console_write_chars(const char *s, size_t len) {
    /* One write stays contiguous on screen, same guarantee kprintf gives. */
    uint64_t flags = spin_lock_irqsave(&console_lock);
    for (size_t i = 0; i < len; i++) console_putc(s[i]);
    spin_unlock_irqrestore(&console_lock, flags);
}

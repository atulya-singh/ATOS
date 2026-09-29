#pragma once
#include <stddef.h>
#include "../lib/spinlock.h"

/* The kernel's text output sink: every character goes to serial (the
 * debugging/CI channel) and to the framebuffer console (the one on screen). */

/* Serializes output from all CPUs, so each kprintf and each console write
 * stays contiguous. console_putc expects it held. */
extern struct spinlock console_lock;

void console_putc(char c);
/* Takes console_lock itself. */
void console_write_chars(const char *s, size_t len);

#pragma once
#include <stddef.h>

/* The kernel's text output sink: every character goes to serial (the
 * debugging/CI channel) and to the framebuffer console (the one on screen). */
void console_putc(char c);
void console_write(const char *s, size_t len);

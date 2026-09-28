#pragma once
#include <stdarg.h>
#include <stddef.h>

/* printf family, without floating point (userspace is built general-
 * registers-only). Supports %d %i %u %x %X %p %c %s %%, the - and 0 flags,
 * a width, and the l / ll / z length modifiers. Output is formatted into
 * a buffer and handed to write() in one call, so a single printf line
 * doesn't interleave with another process's output. */
int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int vdprintf(int fd, const char *fmt, va_list ap);
int putchar(int c);
int puts(const char *s);

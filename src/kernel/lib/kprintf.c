#include "kprintf.h"
#include "../arch/x86_64/cpu.h"
#include "../dev/console.h"
#include <stdarg.h>
#include <stdint.h>

static void print_str(const char *s) {
    while (*s) console_putc(*s++);
}

/* Collects digits LSB-first, then emits padding followed by the digits in
 * MSB-first order -- doing the reversal here means callers never have to
 * know the final string length up front. */
static void print_uint(unsigned long long value, unsigned base, int uppercase,
                        int width, int zero_pad) {
    char buf[32];
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;

    if (value == 0) buf[i++] = '0';
    while (value > 0) {
        buf[i++] = digits[value % base];
        value /= base;
    }

    int pad = width - i;
    while (pad-- > 0) console_putc(zero_pad ? '0' : ' ');
    while (i > 0) console_putc(buf[--i]);
}

static void print_int(long long value) {
    if (value < 0) {
        console_putc('-');
        print_uint((unsigned long long)(-value), 10, 0, 0, 0);
    } else {
        print_uint((unsigned long long)value, 10, 0, 0, 0);
    }
}

void kprintf(const char *fmt, ...) {
    /* Whole-message atomicity: without this, a timer preemption mid-call
     * interleaves two tasks' output character by character. */
    uint64_t flags = irq_save();
    va_list args;
    va_start(args, fmt);

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            console_putc(*p);
            continue;
        }
        p++;

        int alt = 0, zero_pad = 0, width = 0;
        if (*p == '#') { alt = 1; p++; }
        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        int is_long = 0, is_longlong = 0;
        if (*p == 'l') {
            is_long = 1;
            p++;
            if (*p == 'l') { is_longlong = 1; p++; }
        }

        switch (*p) {
        case 'd':
        case 'i': {
            long long v = is_longlong ? va_arg(args, long long)
                         : is_long     ? va_arg(args, long)
                                       : va_arg(args, int);
            print_int(v);
            break;
        }
        case 'u': {
            unsigned long long v = is_longlong ? va_arg(args, unsigned long long)
                                  : is_long     ? va_arg(args, unsigned long)
                                                : va_arg(args, unsigned int);
            print_uint(v, 10, 0, width, zero_pad);
            break;
        }
        case 'x':
        case 'X': {
            unsigned long long v = is_longlong ? va_arg(args, unsigned long long)
                                  : is_long     ? va_arg(args, unsigned long)
                                                : va_arg(args, unsigned int);
            if (alt) print_str("0x");
            print_uint(v, 16, *p == 'X', width, zero_pad);
            break;
        }
        case 'p': {
            void *v = va_arg(args, void *);
            print_str("0x");
            print_uint((unsigned long long)(uintptr_t)v, 16, 0, 16, 1);
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            console_putc(c);
            break;
        }
        case 's': {
            const char *s = va_arg(args, const char *);
            print_str(s ? s : "(null)");
            break;
        }
        case '%':
            console_putc('%');
            break;
        default:
            console_putc('%');
            if (*p) console_putc(*p);
            break;
        }
    }

    va_end(args);
    irq_restore(flags);
}

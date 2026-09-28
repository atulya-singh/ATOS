#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Output sink for the formatter: a bounded buffer that keeps counting
 * past its end, so vsnprintf can report the length it would have needed. */
struct sink {
    char *buf;
    size_t size;
    size_t len;
};

static void emit(struct sink *s, char c) {
    if (s->len + 1 < s->size) s->buf[s->len] = c;
    s->len++;
}

static void emit_padded(struct sink *s, const char *str, size_t n, int width, int left, char pad) {
    int fill = width > (int)n ? width - (int)n : 0;
    if (!left) while (fill-- > 0) emit(s, pad);
    for (size_t i = 0; i < n; i++) emit(s, str[i]);
    if (left) while (fill-- > 0) emit(s, ' ');
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    struct sink s = {buf, size, 0};

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            emit(&s, *p);
            continue;
        }
        p++;

        int left = 0, zero = 0, width = 0, lng = 0;
        for (;; p++) {
            if (*p == '-') left = 1;
            else if (*p == '0') zero = 1;
            else break;
        }
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        while (*p == 'l' || *p == 'z') { lng = 1; p++; }

        char tmp[24];
        size_t n = 0;
        char pad = (zero && !left) ? '0' : ' ';

        switch (*p) {
        case 'd':
        case 'i': {
            long v = lng ? va_arg(ap, long) : va_arg(ap, int);
            unsigned long u = v < 0 ? -(unsigned long)v : (unsigned long)v;
            do tmp[sizeof(tmp) - 1 - n++] = (char)('0' + u % 10); while (u /= 10);
            if (v < 0) {
                if (pad == '0') { emit(&s, '-'); width--; }
                else tmp[sizeof(tmp) - 1 - n++] = '-';
            }
            emit_padded(&s, tmp + sizeof(tmp) - n, n, width, left, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'p': {
            unsigned long u;
            unsigned base = *p == 'u' ? 10 : 16;
            if (*p == 'p') {
                u = (unsigned long)va_arg(ap, void *);
                emit(&s, '0');
                emit(&s, 'x');
            } else {
                u = lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            }
            const char *digits = *p == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            do tmp[sizeof(tmp) - 1 - n++] = digits[u % base]; while (u /= base);
            emit_padded(&s, tmp + sizeof(tmp) - n, n, width, left, pad);
            break;
        }
        case 'c':
            tmp[0] = (char)va_arg(ap, int);
            emit_padded(&s, tmp, 1, width, left, ' ');
            break;
        case 's': {
            const char *str = va_arg(ap, const char *);
            if (!str) str = "(null)";
            emit_padded(&s, str, strlen(str), width, left, ' ');
            break;
        }
        case '%':
            emit(&s, '%');
            break;
        case '\0':
            p--; /* lone trailing '%': stop cleanly */
            break;
        default:
            emit(&s, '%');
            emit(&s, *p);
            break;
        }
    }

    if (size) buf[s.len < size ? s.len : size - 1] = '\0';
    return (int)s.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static int write_all(int fd, const char *buf, size_t len) {
    while (len) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) return -1;
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int vdprintf(int fd, const char *fmt, va_list ap) {
    char buf[1024];
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(buf, sizeof(buf), fmt, copy);
    va_end(copy);
    if (n < 0) return n;
    /* Longer than the stack buffer: truncate rather than allocate, since
     * printf must keep working even when malloc can't. */
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    return write_all(fd, buf, len) == 0 ? n : -1;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(STDOUT_FILENO, fmt, ap);
    va_end(ap);
    return n;
}

int putchar(int c) {
    char ch = (char)c;
    return write(STDOUT_FILENO, &ch, 1) == 1 ? (unsigned char)c : -1;
}

int puts(const char *s) {
    if (write_all(STDOUT_FILENO, s, strlen(s)) < 0) return -1;
    return putchar('\n') < 0 ? -1 : 0;
}

#include <errno.h>
#include <string.h>

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        while (n--) d[n] = s[n];
    }
    return dst;
}

void *memset(void *dst, int c, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) {
        if (*x != *y) return *x - *y;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        if (*a != *b || !*a) return (unsigned char)*a - (unsigned char)*b;
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {}
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = '\0';
    return dst;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strrchr(const char *s, int c) {
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c) last = s;
        if (!*s) return (char *)last;
    }
}

char *strstr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncmp(haystack, needle, n) == 0) return (char *)haystack;
    }
    return n == 0 ? (char *)haystack : NULL;
}

const char *strerror(int err) {
    switch (err) {
    case EPERM:        return "operation not permitted";
    case ENOENT:       return "no such file or directory";
    case ESRCH:        return "no such process";
    case EIO:          return "I/O error";
    case E2BIG:        return "argument list too long";
    case ENOEXEC:      return "not an executable";
    case EBADF:        return "bad file descriptor";
    case ECHILD:       return "no child processes";
    case ENOMEM:       return "out of memory";
    case EFAULT:       return "bad address";
    case EEXIST:       return "file exists";
    case EBUSY:        return "resource busy";
    case EXDEV:        return "cross-device link";
    case ENOTDIR:      return "not a directory";
    case EISDIR:       return "is a directory";
    case EINVAL:       return "invalid argument";
    case EMFILE:       return "too many open files";
    case EFBIG:        return "file too large";
    case ENOSPC:       return "no space left on device";
    case ESPIPE:       return "illegal seek";
    case EROFS:        return "read-only file system";
    case ENAMETOOLONG: return "file name too long";
    case ENOSYS:       return "function not implemented";
    case ENOTEMPTY:    return "directory not empty";
    case ENODEV:       return "no such device";
    case EAGAIN:       return "resource temporarily unavailable";
    case EPIPE:        return "broken pipe";
    case ENOTSOCK:     return "not a socket";
    case EDESTADDRREQ: return "destination address required";
    case EMSGSIZE:     return "message too long";
    case EPROTOTYPE:   return "protocol wrong type for socket";
    case EOPNOTSUPP:   return "operation not supported";
    case EADDRINUSE:   return "address already in use";
    case ENETUNREACH:  return "network unreachable";
    case ECONNRESET:   return "connection reset by peer";
    case EISCONN:      return "already connected";
    case ENOTCONN:     return "not connected";
    case ETIMEDOUT:    return "timed out";
    case ECONNREFUSED: return "connection refused";
    case EHOSTUNREACH: return "host unreachable";
    default:           return "unknown error";
    }
}

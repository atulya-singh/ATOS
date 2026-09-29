#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* hexdump [-n length] [-s skip] [file]: offset, 16 bytes in hex, and
 * their printable ASCII, per line -- the layout of `hexdump -C`. Runs of
 * identical lines collapse into a single "*". Reads stdin without a file. */

static void line(unsigned long off, const unsigned char *b, int n) {
    printf("%08lx ", off);
    for (int i = 0; i < 16; i++) {
        if (i == 8) printf(" ");
        if (i < n) printf(" %02x", b[i]);
        else printf("   ");
    }
    printf("  |");
    for (int i = 0; i < n; i++) putchar(b[i] >= 0x20 && b[i] < 0x7F ? b[i] : '.');
    printf("|\n");
}

int main(int argc, char **argv) {
    long limit = -1, skip = 0;
    int i = 1;
    for (; i + 1 < argc && argv[i][0] == '-'; i += 2) {
        if (strcmp(argv[i], "-n") == 0) limit = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "-s") == 0) skip = atoi(argv[i + 1]);
        else break;
    }
    if (argc - i > 1 || (i < argc && argv[i][0] == '-')) {
        dprintf(STDERR_FILENO, "usage: hexdump [-n length] [-s skip] [file]\n");
        return 2;
    }
    int fd = STDIN_FILENO;
    if (i < argc && (fd = open(argv[i], O_RDONLY)) < 0) {
        dprintf(STDERR_FILENO, "hexdump: %s: %s\n", argv[i], strerror(errno));
        return 1;
    }
    if (skip && lseek(fd, skip, SEEK_SET) < 0) {
        /* Not seekable (stdin): read and discard instead. */
        char junk[256];
        for (long left = skip; left > 0;) {
            long n = read(fd, junk, left < 256 ? (size_t)left : 256);
            if (n <= 0) break;
            left -= n;
        }
    }

    unsigned char cur[16], prev[16];
    int have_prev = 0, starred = 0, n = 0;
    unsigned long off = (unsigned long)skip;
    for (;;) {
        long want = 16 - n;
        if (limit >= 0 && (long)(off - (unsigned long)skip) + n + want > limit) {
            want = limit - (long)(off - (unsigned long)skip) - n;
        }
        long r = want > 0 ? read(fd, cur + n, (size_t)want) : 0;
        if (r < 0) {
            dprintf(STDERR_FILENO, "hexdump: %s\n", strerror(errno));
            return 1;
        }
        n += (int)r;
        if (n < 16 && r > 0) continue; /* short read: fill the line */
        if (n == 0) break;
        if (n == 16 && have_prev && memcmp(cur, prev, 16) == 0) {
            if (!starred) printf("*\n");
            starred = 1;
        } else {
            line(off, cur, n);
            starred = 0;
        }
        memcpy(prev, cur, 16);
        have_prev = 1;
        off += (unsigned long)n;
        if (n < 16) break;
        n = 0;
    }
    printf("%08lx\n", off);
    return 0;
}

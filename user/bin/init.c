/* PID 1-to-be. For now it checks that the userspace runtime works end to
 * end (argv, printf, malloc over brk, file I/O through the VFS) and exits;
 * it grows into the real init once processes can fork and exec. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int check_malloc(void) {
    /* Enough to force several sbrk growths, with frees in between to
     * exercise splitting and coalescing. */
    char *blocks[64];
    for (int i = 0; i < 64; i++) {
        blocks[i] = malloc(4096 + (size_t)i * 16);
        if (!blocks[i]) return 0;
        memset(blocks[i], i, 4096);
    }
    for (int i = 0; i < 64; i += 2) free(blocks[i]);
    char *big = malloc(200 * 1024);
    if (!big) return 0;
    big[200 * 1024 - 1] = 1;
    for (int i = 1; i < 64; i += 2) {
        if (blocks[i][0] != i || blocks[i][4095] != i) return 0;
        free(blocks[i]);
    }
    free(big);
    return 1;
}

static void show_motd(void) {
    int fd = open("/etc/motd", O_RDONLY);
    if (fd < 0) {
        printf("init: /etc/motd: %s\n", strerror(errno));
        return;
    }
    char buf[256];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(STDOUT_FILENO, buf, (size_t)n);
    close(fd);
}

int main(int argc, char **argv) {
    printf("init: running as %s (argc=%d)\n", argv[0], argc);
    printf("init: malloc/free over brk %s\n", check_malloc() ? "ok" : "FAILED");
    if (open("/no/such/file", O_RDONLY) < 0 && errno == ENOENT) {
        printf("init: errno reporting ok\n");
    }
    show_motd();
    return 0;
}

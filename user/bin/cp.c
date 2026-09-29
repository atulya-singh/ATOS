#include <atos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* cp src dst: copies one file. If dst is an existing directory, the copy
 * goes into it under src's name. */

static int is_dir(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct atos_stat st;
    int r = fstat(fd, &st) == 0 && st.type == ATOS_TYPE_DIR;
    close(fd);
    return r;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        dprintf(STDERR_FILENO, "usage: cp src dst\n");
        return 2;
    }
    char target[256];
    const char *dst = argv[2];
    if (is_dir(dst)) {
        const char *base = strrchr(argv[1], '/');
        snprintf(target, sizeof(target), "%s/%s", dst, base ? base + 1 : argv[1]);
        dst = target;
    }
    int in = open(argv[1], O_RDONLY);
    if (in < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    static char buf[16384];
    ssize_t n;
    int status = 0;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n) {
            n = -1;
            break;
        }
    }
    if (n < 0) {
        dprintf(STDERR_FILENO, "cp: %s: %s\n", argv[1], strerror(errno));
        status = 1;
    }
    close(in);
    close(out);
    return status;
}

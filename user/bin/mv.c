#include <atos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* mv src dst: renames within one filesystem. If dst is an existing
 * directory, src moves into it under its own name. */

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
        dprintf(STDERR_FILENO, "usage: mv src dst\n");
        return 2;
    }
    char target[256];
    const char *dst = argv[2];
    if (is_dir(dst)) {
        const char *base = strrchr(argv[1], '/');
        snprintf(target, sizeof(target), "%s/%s", dst, base ? base + 1 : argv[1]);
        dst = target;
    }
    if (rename(argv[1], dst) < 0) {
        dprintf(STDERR_FILENO, "mv: %s -> %s: %s\n", argv[1], dst, strerror(errno));
        return 1;
    }
    return 0;
}

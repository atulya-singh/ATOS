#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int cat_fd(int fd) {
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write(STDOUT_FILENO, buf, (size_t)n) != n) return -1;
    }
    return n < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: cat file...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0 || cat_fd(fd) < 0) {
            dprintf(STDERR_FILENO, "cat: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
        if (fd >= 0) close(fd);
    }
    return status;
}

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Prints "lines words bytes name" for each file, like POSIX wc. */
static int count(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "wc: %s: %s\n", path, strerror(errno));
        return 1;
    }
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        bytes += (unsigned long)n;
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n') lines++;
            int space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
            if (!space && !in_word) words++;
            in_word = !space;
        }
    }
    close(fd);
    if (n < 0) {
        dprintf(STDERR_FILENO, "wc: %s: %s\n", path, strerror(errno));
        return 1;
    }
    printf("%lu %lu %lu %s\n", lines, words, bytes, path);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: wc file...\n");
        return 1;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) status |= count(argv[i]);
    return status;
}

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* mkdir [-p] dir...: -p creates missing parents and is quiet about
 * directories that already exist. */

static int make_parents(char *path) {
    for (char *p = path + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        int r = mkdir(path, 0755);
        *p = '/';
        if (r < 0 && errno != EEXIST) return -1;
    }
    return mkdir(path, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

int main(int argc, char **argv) {
    int parents = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-p") == 0) {
        parents = 1;
        i++;
    }
    if (i == argc) {
        dprintf(STDERR_FILENO, "usage: mkdir [-p] dir...\n");
        return 2;
    }
    int status = 0;
    for (; i < argc; i++) {
        int r = parents ? make_parents(argv[i]) : mkdir(argv[i], 0755);
        if (r < 0) {
            dprintf(STDERR_FILENO, "mkdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}

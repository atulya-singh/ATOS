#include <atos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* rm [-r] path...: -r removes directories and everything under them. */

static int remove_tree(const char *path) {
    if (unlink(path) == 0) return 0;
    if (errno != EISDIR) return -1;
    /* Empty the directory one entry at a time: removing entry 0 moves
     * the next one into its place. The directory is closed before each
     * removal, since the filesystem won't remove what is open. */
    char child[256];
    for (;;) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) return -1;
        struct atos_dirent ent;
        int r = readdir(fd, 0, &ent);
        close(fd);
        if (r < 0) break;
        snprintf(child, sizeof(child), "%s/%s", path, ent.name);
        if (remove_tree(child) < 0) {
            dprintf(STDERR_FILENO, "rm: %s: %s\n", child, strerror(errno));
            return -1;
        }
    }
    return rmdir(path);
}

int main(int argc, char **argv) {
    int recursive = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-r") == 0) {
        recursive = 1;
        i++;
    }
    if (i == argc) {
        dprintf(STDERR_FILENO, "usage: rm [-r] path...\n");
        return 2;
    }
    int status = 0;
    for (; i < argc; i++) {
        if ((recursive ? remove_tree(argv[i]) : unlink(argv[i])) < 0) {
            dprintf(STDERR_FILENO, "rm: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: rmdir dir...\n");
        return 2;
    }
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) < 0) {
            dprintf(STDERR_FILENO, "rmdir: %s: %s\n", argv[i], strerror(errno));
            status = 1;
        }
    }
    return status;
}

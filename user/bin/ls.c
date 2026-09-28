#include <atos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int list(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct atos_stat st;
    if (fstat(fd, &st) == 0 && st.type != ATOS_TYPE_DIR) {
        printf("%8lu  %s\n", (unsigned long)st.size, path);
        close(fd);
        return 0;
    }

    struct atos_dirent ent;
    for (unsigned long i = 0; readdir(fd, i, &ent) == 0; i++) {
        switch (ent.type) {
        case ATOS_TYPE_DIR:     printf("   <dir>  %s/\n", ent.name); break;
        case ATOS_TYPE_CHARDEV: printf("   <dev>  %s\n", ent.name); break;
        default:                printf("%8lu  %s\n", (unsigned long)ent.size, ent.name); break;
        }
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return list("/");
    int status = 0;
    for (int i = 1; i < argc; i++) {
        if (argc > 2) printf("%s:\n", argv[i]);
        status |= list(argv[i]);
    }
    return status;
}

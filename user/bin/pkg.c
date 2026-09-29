#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* pkg list | pkg info <port>: shows the ports built into this system
 * image (see docs/ports.md). The build writes one line per port to
 * /usr/share/ports/INDEX ("name version description") and a metadata file
 * per port, /usr/share/ports/<name>, listing what it installed. */

#define PORTS_DIR "/usr/share/ports"

static long slurp(const char *path, char *buf, size_t cap) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    long len = 0, n;
    while (len < (long)cap - 1 && (n = read(fd, buf + len, cap - 1 - (size_t)len)) > 0) len += n;
    close(fd);
    buf[len] = '\0';
    return len;
}

static int list(void) {
    static char index[8192];
    if (slurp(PORTS_DIR "/INDEX", index, sizeof(index)) < 0) {
        dprintf(STDERR_FILENO, "pkg: %s/INDEX: %s\n", PORTS_DIR, strerror(errno));
        return 1;
    }
    int count = 0;
    for (char *line = index; *line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        /* name, version, then the description (which has spaces) */
        char *ver = strchr(line, ' ');
        char *desc = ver ? strchr(ver + 1, ' ') : NULL;
        if (desc) {
            *ver++ = '\0';
            *desc++ = '\0';
            printf("%-10s %-6s %s\n", line, ver, desc);
            count++;
        }
        if (!nl) break;
        line = nl + 1;
    }
    printf("%d port(s) installed\n", count);
    return 0;
}

static int info(const char *name) {
    if (strchr(name, '/') || strcmp(name, "INDEX") == 0) {
        dprintf(STDERR_FILENO, "pkg: bad port name '%s'\n", name);
        return 2;
    }
    char path[128];
    snprintf(path, sizeof(path), PORTS_DIR "/%s", name);
    static char meta[4096];
    if (slurp(path, meta, sizeof(meta)) < 0) {
        dprintf(STDERR_FILENO, "pkg: %s: not installed\n", name);
        return 1;
    }
    printf("%s", meta);
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "list") == 0) return list();
    if (argc == 3 && strcmp(argv[1], "info") == 0) return info(argv[2]);
    dprintf(STDERR_FILENO, "usage: pkg list | pkg info <port>\n");
    return 2;
}

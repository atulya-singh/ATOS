#include <atos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* fortune [-n index]: prints one entry of /usr/share/fortune/fortunes
 * (entries separated by lines holding just "%"), chosen by the clock, or
 * entry `index` (from 1) with -n. */

#define FORTUNES "/usr/share/fortune/fortunes"

int main(int argc, char **argv) {
    long pick = -1;
    if (argc == 3 && strcmp(argv[1], "-n") == 0) pick = atoi(argv[2]);
    else if (argc != 1) {
        dprintf(STDERR_FILENO, "usage: fortune [-n index]\n");
        return 2;
    }
    int fd = open(FORTUNES, O_RDONLY);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "fortune: %s: %s\n", FORTUNES, strerror(errno));
        return 1;
    }
    static char text[16384];
    long len = 0, n;
    while (len < (long)sizeof(text) - 1 && (n = read(fd, text + len, sizeof(text) - 1 - (size_t)len)) > 0) {
        len += n;
    }
    close(fd);
    text[len] = '\0';

    /* Split in place: each "\n%\n" ends an entry. */
    char *entries[256];
    int count = 0;
    char *s = text;
    while (*s && count < 256) {
        entries[count++] = s;
        char *sep = strstr(s, "\n%\n");
        if (!sep) break;
        sep[1] = '\0';
        s = sep + 3;
    }
    if (count == 0) return 1;
    long i = pick > 0 ? (pick - 1) % count : (long)(uptime_ms() / 10 % (unsigned long)count);
    printf("%s", entries[i]);
    size_t l = strlen(entries[i]);
    if (l && entries[i][l - 1] != '\n') printf("\n");
    return 0;
}

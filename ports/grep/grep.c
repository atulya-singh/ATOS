#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "regex.h"

/* grep [-invc] pattern [file...]: prints the lines matching `pattern` (see
 * regex.h for the syntax), prefixed by the file name when searching more
 * than one file. Exit status 0 if anything matched, 1 if not, 2 on error. */

static int icase, invert, numbers, count_only, show_names;
static const char *pattern;

#define LINE_CAP 1024

static long search(int fd, const char *name) {
    static char buf[4096];
    char line[LINE_CAP];
    size_t len = 0;
    long lineno = 0, matches = 0;
    long n;
    int eof = 0;
    while (!eof) {
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            dprintf(STDERR_FILENO, "grep: %s: %s\n", name, strerror(errno));
            return -1;
        }
        if (n == 0) {
            eof = 1;
            if (len == 0) break;
            buf[0] = '\n'; /* finish a last line with no newline */
            n = 1;
        }
        for (long i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (len < LINE_CAP - 1) line[len++] = buf[i]; /* overlong lines are cut */
                continue;
            }
            line[len] = '\0';
            lineno++;
            if (regex_search(pattern, line, icase) != invert) {
                matches++;
                if (!count_only) {
                    if (show_names) printf("%s:", name);
                    if (numbers) printf("%ld:", lineno);
                    printf("%s\n", line);
                }
            }
            len = 0;
        }
    }
    if (count_only) {
        if (show_names) printf("%s:", name);
        printf("%ld\n", matches);
    }
    return matches;
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *f = argv[i] + 1; *f; f++) {
            switch (*f) {
            case 'i': icase = 1; break;
            case 'v': invert = 1; break;
            case 'n': numbers = 1; break;
            case 'c': count_only = 1; break;
            default:
                dprintf(STDERR_FILENO, "grep: unknown option -%c\n", *f);
                return 2;
            }
        }
    }
    if (i >= argc) {
        dprintf(STDERR_FILENO, "usage: grep [-invc] pattern [file...]\n");
        return 2;
    }
    pattern = argv[i++];
    const char *err = regex_check(pattern);
    if (err) {
        dprintf(STDERR_FILENO, "grep: bad pattern: %s\n", err);
        return 2;
    }
    show_names = argc - i > 1;

    long total = 0;
    int failed = 0;
    if (i == argc) {
        total = search(STDIN_FILENO, "(standard input)");
        failed = total < 0;
    }
    for (; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(STDERR_FILENO, "grep: %s: %s\n", argv[i], strerror(errno));
            failed = 1;
            continue;
        }
        long m = search(fd, argv[i]);
        close(fd);
        if (m < 0) failed = 1;
        else total += m;
    }
    if (failed) return 2;
    return total ? 0 : 1;
}

/* ATOS shell: reads a line (echoing and editing it itself, since the
 * console is raw), splits it into words, and runs the command with
 * fork + exec + waitpid. Supports "double quotes" and output redirection
 * with > and >>. Commands without a '/' are looked up in /bin. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define LINE_MAX 256
#define MAX_ARGS 32

static int read_line(char *line, size_t size) {
    size_t len = 0;
    for (;;) {
        char c;
        if (read(STDIN_FILENO, &c, 1) != 1) return -1;
        if (c == '\n' || c == '\r') {
            write(STDOUT_FILENO, "\n", 1);
            line[len] = '\0';
            return (int)len;
        }
        if (c == '\b' || c == 0x7F) {
            if (len) {
                len--;
                write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        if (c == 0x15) { /* ^U: kill the whole line */
            while (len) {
                len--;
                write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        if ((unsigned char)c < ' ' || len + 1 >= size) continue;
        line[len++] = c;
        write(STDOUT_FILENO, &c, 1);
    }
}

/* Splits `line` in place. Quoted runs keep their spaces; the quotes go. */
static int split_words(char *line, char **words, int max) {
    int n = 0;
    char *src = line, *dst = line;
    while (*src) {
        while (*src == ' ' || *src == '\t') src++;
        if (!*src) break;
        if (n == max - 1) return -1;
        words[n++] = dst;
        int quoted = 0;
        while (*src && (quoted || (*src != ' ' && *src != '\t'))) {
            if (*src == '"') quoted = !quoted;
            else *dst++ = *src;
            src++;
        }
        if (*src) src++;
        *dst++ = '\0';
    }
    words[n] = NULL;
    return n;
}

static void help(void) {
    printf("Built-ins: help, exit [code]\n"
           "Programs (in /bin): ls [dir], cat file..., echo words..., libctest\n"
           "Redirect output with > file (truncate) or >> file (append).\n");
}

/* In the child: apply redirections, then become the command. */
static void run_child(char **argv, const char *out_path, int append) {
    if (out_path) {
        int fd = open(out_path, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC));
        if (fd < 0) {
            printf("sh: %s: %s\n", out_path, strerror(errno));
            _exit(1);
        }
        dup2(fd, STDOUT_FILENO);
        close(fd);
    }
    char path[LINE_MAX];
    if (strchr(argv[0], '/')) {
        snprintf(path, sizeof(path), "%s", argv[0]);
    } else {
        snprintf(path, sizeof(path), "/bin/%s", argv[0]);
    }
    execv(path, argv);
    dprintf(STDERR_FILENO, "sh: %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

static void run(char **words, int n) {
    const char *out_path = NULL;
    int append = 0;
    int argc = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(words[i], ">") == 0 || strcmp(words[i], ">>") == 0) {
            if (i + 1 >= n) {
                printf("sh: missing file name after %s\n", words[i]);
                return;
            }
            append = words[i][1] == '>';
            out_path = words[++i];
        } else {
            words[argc++] = words[i];
        }
    }
    words[argc] = NULL;
    if (argc == 0) return;

    if (strcmp(words[0], "help") == 0) {
        help();
        return;
    }
    if (strcmp(words[0], "exit") == 0) exit(argc > 1 ? atoi(words[1]) : 0);

    pid_t pid = fork();
    if (pid < 0) {
        printf("sh: fork: %s\n", strerror(errno));
        return;
    }
    if (pid == 0) run_child(words, out_path, append);

    int status = 0;
    waitpid(pid, &status, 0);
    if (status != 0 && status != 127) printf("[exit %d]\n", status);
}

int main(void) {
    char line[LINE_MAX];
    char *words[MAX_ARGS];
    for (;;) {
        printf("atos$ ");
        if (read_line(line, sizeof(line)) < 0) return 1;
        int n = split_words(line, words, MAX_ARGS);
        if (n < 0) {
            printf("sh: too many arguments\n");
            continue;
        }
        run(words, n);
    }
}

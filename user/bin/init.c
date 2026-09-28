/* The first user process, started by the kernel once boot self-tests
 * pass. Shows the message of the day, then keeps a shell running: when
 * the shell exits, init reports how and starts a fresh one. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void show_motd(void) {
    int fd = open("/etc/motd", O_RDONLY);
    if (fd < 0) return;
    char buf[256];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) write(STDOUT_FILENO, buf, (size_t)n);
    close(fd);
}

int main(void) {
    show_motd();
    for (;;) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("init: fork failed: %s\n", strerror(errno));
            return 1;
        }
        if (pid == 0) {
            char *argv[] = {"/bin/sh", NULL};
            execv(argv[0], argv);
            printf("init: cannot run /bin/sh: %s\n", strerror(errno));
            _exit(127);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        printf("init: shell exited with code %d, restarting\n", status);
    }
}

/* Userspace runtime self-test: libc (printf, malloc over brk, errno) and
 * the process syscalls (fork, exec, waitpid, getpid, dup2). Prints one
 * "ok"/"FAILED" line per check; tools/smoke-test.sh runs it from the shell
 * and greps for the ok lines. Exit code = number of failures. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

static void report(const char *what, int ok) {
    printf("libctest: %s %s\n", what, ok ? "ok" : "FAILED");
    if (!ok) failures++;
}

static int check_printf(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d|%5s|%-3c|%04x|%lu|%%", -42, "ab", 'z', 0xbeef, 1234567890123ul);
    return strcmp(buf, "-42|   ab|z  |beef|1234567890123|%") == 0;
}

static int check_malloc(void) {
    /* Enough to force several sbrk growths, with frees in between to
     * exercise splitting and coalescing. */
    char *blocks[64];
    for (int i = 0; i < 64; i++) {
        blocks[i] = malloc(4096 + (size_t)i * 16);
        if (!blocks[i]) return 0;
        memset(blocks[i], i, 4096);
    }
    for (int i = 0; i < 64; i += 2) free(blocks[i]);
    char *big = malloc(200 * 1024);
    if (!big) return 0;
    big[200 * 1024 - 1] = 1;
    for (int i = 1; i < 64; i += 2) {
        if (blocks[i][0] != i || blocks[i][4095] != i) return 0;
        free(blocks[i]);
    }
    free(big);
    return 1;
}

static int check_fork_wait(void) {
    int shared = 1;
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        /* The child has its own copy of memory: this write stays here. */
        shared = 99;
        _exit(getpid() != parent && shared == 99 ? 7 : 1);
    }
    int status = -1;
    pid_t got = waitpid(pid, &status, 0);
    return got == pid && status == 7 && shared == 1 &&
           waitpid(-1, &status, 0) < 0 && errno == ECHILD;
}

static int check_exec(void) {
    char *argv[] = {"/bin/nonexistent", NULL};
    if (execv(argv[0], argv) == 0 || errno != ENOENT) return 0;
    argv[0] = "/etc/motd"; /* not an ELF */
    if (execv(argv[0], argv) == 0 || errno != ENOEXEC) return 0;

    pid_t pid = fork();
    if (pid == 0) {
        char *echo_argv[] = {"echo", "libctest:", "exec", "argv", "passing", "ok", NULL};
        execv("/bin/echo", echo_argv);
        _exit(1);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return status == 0;
}

/* noinline: the test writes to this function's own code, so it must
 * exist as a real function rather than be folded into main. */
__attribute__((noinline)) static int check_wx(void) {
    /* Writing to our own code must kill the process (#PF, 128 + 14),
     * so try it in a child. */
    pid_t pid = fork();
    if (pid == 0) {
        volatile char *text = (volatile char *)check_wx;
        *text = 0xC3;
        _exit(0);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return status == 142;
}

static int check_dup2(void) {
    /* Redirect stdout into /dev/null in a child: nothing it prints may
     * reach the console, and it still exits normally. */
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) != STDOUT_FILENO) _exit(1);
        close(fd);
        printf("libctest: this line must not appear\n");
        _exit(0);
    }
    int status = -1;
    waitpid(pid, &status, 0);
    return status == 0;
}

static int check_float_printf(void) {
    char buf[96];
    snprintf(buf, sizeof(buf), "%.3f|%f|%g|%8.2f|%.0f|%-6.1f|%05.1f", 3.14159, -0.5, 2.5, 1234.567,
             2.6, 1.25, -2.0);
    return strcmp(buf, "3.142|-0.500000|2.5| 1234.57|3|1.3   |-02.0") == 0;
}

static int check_strtod(void) {
    char *end;
    double v = strtod("  -12.5e2xyz", &end);
    return v == -1250.0 && strcmp(end, "xyz") == 0 && fabs(atof("0.001") - 0.001) < 1e-15 &&
           atof("abc") == 0;
}

static int near(double a, double b) { return fabs(a - b) < 1e-9; }

static int check_libm(void) {
    return near(sqrt(2), 1.4142135623730951) && near(sin(M_PI / 2), 1) && near(cos(0), 1) &&
           near(exp(log(10)), 10) && pow(2, 10) == 1024 && pow(-2, 3) == -8 && floor(-1.5) == -2 &&
           ceil(1.2) == 2 && round(2.5) == 3 && near(fmod(7.5, 2), 1.5) &&
           near(atan2(1, 1), M_PI / 4) && near(log10(1000), 3) && near(tan(M_PI / 4), 1) &&
           isnan(sqrt(-1));
}

/* A long register-resident loop under a per-seed SSE rounding mode
 * (MXCSR bits 13-14), with an occasional x87 sin. With more copies than
 * CPUs, timer preemption lands mid-loop: if the kernel didn't save and
 * restore FPU/SSE state, a copy would resume with another's registers or
 * rounding mode and get a different answer. */
static double crunch(int seed) {
    unsigned saved, mxcsr;
    asm volatile("stmxcsr %0" : "=m"(saved));
    mxcsr = (saved & ~0x6000u) | (unsigned)(seed % 4) << 13;
    asm volatile("ldmxcsr %0" : : "m"(mxcsr));
    double a = seed + 0.25, b = 1.0 / (seed + 3), c = 0;
    for (int i = 1; i <= 400000; i++) {
        a = a * 1.0000001 + b;
        b = b * 0.9999999 + a * 1e-9;
        c += a / i;
        if ((i & 0xFFFF) == 0) c += sin(c);
    }
    asm volatile("ldmxcsr %0" : : "m"(saved));
    return a + b + c;
}

static int check_fpu_switch(void) {
    enum { N = 8 };
    double expect[N];
    for (int k = 0; k < N; k++) expect[k] = crunch(k);
    pid_t pids[N];
    for (int k = 0; k < N; k++) {
        pids[k] = fork();
        if (pids[k] == 0) _exit(crunch(k) == expect[k] ? 0 : 1);
    }
    int ok = 1;
    for (int k = 0; k < N; k++) {
        int status = -1;
        waitpid(pids[k], &status, 0);
        if (status != 0) ok = 0;
    }
    return ok;
}

int main(int argc, char **argv) {
    /* The smoke test runs us as "libctest one two". */
    report("argv", argc == 3 && strcmp(argv[0], "libctest") == 0 && strcmp(argv[1], "one") == 0 &&
                   strcmp(argv[2], "two") == 0 && argv[3] == NULL);
    report("printf formatting", check_printf());
    report("malloc/free over brk", check_malloc());
    report("errno", open("/no/such/file", O_RDONLY) < 0 && errno == ENOENT);
    report("fork/waitpid/getpid", check_fork_wait());
    report("exec errors", check_exec());
    report("write to .text kills the process", check_wx());
    report("dup2 redirection", check_dup2());
    report("float printf", check_float_printf());
    report("strtod", check_strtod());
    report("libm", check_libm());
    report("FPU state across context switches", check_fpu_switch());
    printf("libctest: %d failure(s)\n", failures);
    return failures;
}

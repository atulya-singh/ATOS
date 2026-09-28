#include <atos.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

int errno;

/* int 0x80 with up to three arguments; see include/atos/abi.h. The kernel
 * preserves every register except %rax. */
static inline long syscall3(long n, long a0, long a1, long a2) {
    long ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(n), "D"(a0), "S"(a1), "d"(a2)
                 : "memory");
    return ret;
}

/* Kernel errors come back as -errno; POSIX wants -1 + errno. */
static long check(long ret) {
    if (ret < 0) {
        errno = (int)-ret;
        return -1;
    }
    return ret;
}

ssize_t read(int fd, void *buf, size_t len) {
    return check(syscall3(SYS_READ, fd, (long)buf, (long)len));
}

ssize_t write(int fd, const void *buf, size_t len) {
    return check(syscall3(SYS_WRITE, fd, (long)buf, (long)len));
}

int open(const char *path, int flags) {
    return (int)check(syscall3(SYS_OPEN, (long)path, flags, 0));
}

int close(int fd) {
    return (int)check(syscall3(SYS_CLOSE, fd, 0, 0));
}

off_t lseek(int fd, off_t offset, int whence) {
    return check(syscall3(SYS_SEEK, fd, offset, whence));
}

int readdir(int fd, unsigned long index, struct atos_dirent *ent) {
    return (int)check(syscall3(SYS_READDIR, fd, (long)index, (long)ent));
}

int fstat(int fd, struct atos_stat *st) {
    return (int)check(syscall3(SYS_FSTAT, fd, (long)st, 0));
}

int sched_yield(void) {
    return (int)syscall3(SYS_YIELD, 0, 0, 0);
}

void *sbrk(intptr_t increment) {
    static unsigned long cur;
    if (!cur) cur = (unsigned long)syscall3(SYS_BRK, 0, 0, 0);
    unsigned long old = cur;
    if (increment == 0) return (void *)old;
    unsigned long want = old + (unsigned long)increment;
    unsigned long got = (unsigned long)syscall3(SYS_BRK, (long)want, 0, 0);
    if (got != want) {
        errno = ENOMEM;
        return (void *)-1;
    }
    cur = got;
    return (void *)old;
}

pid_t fork(void) {
    return check(syscall3(SYS_FORK, 0, 0, 0));
}

int execv(const char *path, char *const argv[]) {
    return (int)check(syscall3(SYS_EXEC, (long)path, (long)argv, 0));
}

pid_t waitpid(pid_t pid, int *status, int options) {
    (void)options; /* no WNOHANG etc. yet */
    return check(syscall3(SYS_WAITPID, pid, (long)status, 0));
}

pid_t getpid(void) {
    return syscall3(SYS_GETPID, 0, 0, 0);
}

int dup2(int oldfd, int newfd) {
    return (int)check(syscall3(SYS_DUP2, oldfd, newfd, 0));
}

void _exit(int code) {
    syscall3(SYS_EXIT, code, 0, 0);
    __builtin_unreachable();
}

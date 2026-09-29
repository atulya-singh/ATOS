#include <atos.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int errno;

/* int 0x80 with up to four arguments; see include/atos/abi.h. The kernel
 * preserves every register except %rax. */
static inline long syscall4(long n, long a0, long a1, long a2, long a3) {
    register long r10 asm("r10") = a3;
    long ret;
    asm volatile("int $0x80"
                 : "=a"(ret)
                 : "a"(n), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
                 : "memory");
    return ret;
}

static inline long syscall3(long n, long a0, long a1, long a2) {
    return syscall4(n, a0, a1, a2, 0);
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

int mkdir(const char *path, mode_t mode) {
    (void)mode;
    return (int)check(syscall3(SYS_MKDIR, (long)path, 0, 0));
}

int unlink(const char *path) {
    return (int)check(syscall3(SYS_UNLINK, (long)path, 0, 0));
}

int rmdir(const char *path) {
    return (int)check(syscall3(SYS_RMDIR, (long)path, 0, 0));
}

int rename(const char *old_path, const char *new_path) {
    return (int)check(syscall3(SYS_RENAME, (long)old_path, (long)new_path, 0));
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

int reboot(int how) {
    return (int)check(syscall3(SYS_REBOOT, how, 0, 0));
}

/* --- sockets --- */

int socket(int domain, int type, int protocol) {
    if (domain != AF_INET) {
        errno = EINVAL;
        return -1;
    }
    int kind;
    if (type == SOCK_STREAM && (protocol == 0 || protocol == IPPROTO_TCP)) kind = ATOS_SOCK_STREAM;
    else if (type == SOCK_DGRAM && (protocol == 0 || protocol == IPPROTO_UDP)) kind = ATOS_SOCK_DGRAM;
    else if (type == SOCK_DGRAM && protocol == IPPROTO_ICMP) kind = ATOS_SOCK_ICMP;
    else {
        errno = EPROTOTYPE;
        return -1;
    }
    return (int)check(syscall3(SYS_SOCKET, kind, 0, 0));
}

static int addr_ok(const struct sockaddr *addr, socklen_t len) {
    if (!addr || len < sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return 0;
    }
    return 1;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
    if (!addr_ok(addr, len)) return -1;
    return (int)check(syscall3(SYS_BIND, fd, (long)addr, 0));
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    if (!addr_ok(addr, len)) return -1;
    return (int)check(syscall3(SYS_CONNECT, fd, (long)addr, 0));
}

int listen(int fd, int backlog) {
    return (int)check(syscall3(SYS_LISTEN, fd, backlog, 0));
}

/* The kernel always fills a whole sockaddr_in; callers with a smaller
 * buffer get a truncated copy, as POSIX says. */
static void copy_addr_out(struct sockaddr *dst, socklen_t *len, const struct sockaddr_in *src) {
    if (!dst || !len) return;
    socklen_t n = *len < sizeof(*src) ? *len : sizeof(*src);
    memcpy(dst, src, n);
    *len = sizeof(*src);
}

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
    struct sockaddr_in peer;
    int nfd = (int)check(syscall3(SYS_ACCEPT, fd, (long)&peer, 0));
    if (nfd >= 0) copy_addr_out(addr, len, &peer);
    return nfd;
}

long sendto(int fd, const void *buf, size_t len, int flags,
            const struct sockaddr *dest, socklen_t dest_len) {
    (void)flags;
    if (dest && !addr_ok(dest, dest_len)) return -1;
    return check(syscall4(SYS_SENDTO, fd, (long)buf, (long)len, (long)dest));
}

long recvfrom(int fd, void *buf, size_t len, int flags,
              struct sockaddr *src, socklen_t *src_len) {
    (void)flags;
    struct sockaddr_in from;
    long n = check(syscall4(SYS_RECVFROM, fd, (long)buf, (long)len, (long)&from));
    if (n >= 0) copy_addr_out(src, src_len, &from);
    return n;
}

long send(int fd, const void *buf, size_t len, int flags) {
    return sendto(fd, buf, len, flags, NULL, 0);
}

long recv(int fd, void *buf, size_t len, int flags) {
    return recvfrom(fd, buf, len, flags, NULL, NULL);
}

int setsockopt(int fd, int level, int name, const void *value, socklen_t len) {
    if (level != SOL_SOCKET || name != SO_RCVTIMEO || len < sizeof(struct timeval)) {
        errno = EINVAL;
        return -1;
    }
    const struct timeval *tv = value;
    long ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;
    return (int)check(syscall3(SYS_SOCKOPT, fd, ATOS_SO_RCVTIMEO, ms));
}

int netinfo(struct atos_netinfo *info) {
    return (int)check(syscall3(SYS_NETINFO, (long)info, 0, 0));
}

unsigned long uptime_ms(void) {
    return (unsigned long)syscall3(SYS_UPTIME, 0, 0, 0);
}

int msleep(unsigned long ms) {
    return (int)syscall3(SYS_SLEEP, (long)ms, 0, 0);
}

unsigned sleep(unsigned seconds) {
    msleep(seconds * 1000UL);
    return 0;
}

void _exit(int code) {
    syscall3(SYS_EXIT, code, 0, 0);
    __builtin_unreachable();
}

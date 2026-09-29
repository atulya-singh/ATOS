/* The kernel <-> userspace ABI: syscall numbers, flag values, errno codes,
 * and the structs syscalls exchange. Included by both the kernel and the
 * userspace libc, so the two can't drift apart. Plain C, no kernel types. */
#pragma once
#include <stdint.h>

/* int 0x80: number in %rax, arguments in %rdi, %rsi, %rdx, %r10, %r8
 * (Linux's syscall order, so a future syscall/sysret path keeps the same
 * layout), result in %rax. Failures return a negative errno. */
#define SYS_WRITE   0  /* write(fd, buf, len) -> bytes written */
#define SYS_EXIT    1  /* exit(code) -> does not return */
#define SYS_YIELD   2  /* yield() -> 0 */
#define SYS_READ    3  /* read(fd, buf, len) -> bytes read, 0 at EOF */
#define SYS_OPEN    4  /* open(path, flags) -> fd */
#define SYS_CLOSE   5  /* close(fd) -> 0 */
#define SYS_SEEK    6  /* seek(fd, offset, whence) -> new offset */
#define SYS_READDIR 7  /* readdir(fd, index, struct atos_dirent *) -> 0, or -ENOENT past the end */
#define SYS_FSTAT   8  /* fstat(fd, struct atos_stat *) -> 0 */
#define SYS_BRK     9  /* brk(addr) -> new break (the old one on failure; 0 queries) */
#define SYS_FORK    10 /* fork() -> child pid in the parent, 0 in the child */
#define SYS_EXEC    11 /* exec(path, argv) -> does not return on success */
#define SYS_WAITPID 12 /* waitpid(pid or -1, int *status) -> pid of the exited child */
#define SYS_GETPID  13 /* getpid() -> pid */
#define SYS_DUP2    14 /* dup2(oldfd, newfd) -> newfd */
#define SYS_REBOOT  15 /* reboot(ATOS_REBOOT_*) -> does not return on success */
#define SYS_SOCKET  16 /* socket(ATOS_SOCK_*) -> fd */
#define SYS_BIND    17 /* bind(fd, const struct atos_sockaddr_in *) -> 0 */
#define SYS_CONNECT 18 /* connect(fd, const struct atos_sockaddr_in *) -> 0 */
#define SYS_LISTEN  19 /* listen(fd, backlog) -> 0 */
#define SYS_ACCEPT  20 /* accept(fd, struct atos_sockaddr_in *peer or NULL) -> new fd */
#define SYS_SENDTO  21 /* sendto(fd, buf, len, const struct atos_sockaddr_in *dest or NULL) -> bytes sent */
#define SYS_RECVFROM 22 /* recvfrom(fd, buf, len, struct atos_sockaddr_in *src or NULL) -> bytes, 0 at EOF */
#define SYS_SOCKOPT 23 /* sockopt(fd, ATOS_SO_*, value) -> 0 */
#define SYS_NETINFO 24 /* netinfo(struct atos_netinfo *) -> 0, or -ENODEV without a NIC */
#define SYS_UPTIME  25 /* uptime() -> milliseconds since boot (timer-tick resolution) */
#define SYS_SLEEP   26 /* sleep(ms) -> 0 */
#define SYS_MKDIR   27 /* mkdir(path) -> 0 */
#define SYS_UNLINK  28 /* unlink(path) -> 0; files only */
#define SYS_RMDIR   29 /* rmdir(path) -> 0; empty directories only */
#define SYS_RENAME  30 /* rename(old, new) -> 0; replaces an existing file at new */

#define ATOS_REBOOT_POWEROFF 0
#define ATOS_REBOOT_RESTART  1

/* open() flags: Linux's values, for familiarity. */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_ACCMODE 0x0003
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define ATOS_TYPE_FILE    1
#define ATOS_TYPE_DIR     2
#define ATOS_TYPE_CHARDEV 3
#define ATOS_TYPE_SOCKET  4

/* Socket types. ICMP sockets send and receive ICMP echo messages (header
 * included) like Linux's unprivileged ping sockets: the kernel fills in the
 * identifier and checksum on the way out and delivers only the replies
 * that match on the way in. */
#define ATOS_SOCK_STREAM 1 /* TCP */
#define ATOS_SOCK_DGRAM  2 /* UDP */
#define ATOS_SOCK_ICMP   3

/* sockopt options. */
#define ATOS_SO_RCVTIMEO 1 /* receive/accept/connect timeout in ms; 0 = wait forever */

#define ATOS_AF_INET 2

/* Laid out like BSD's sockaddr_in; port and addr in network byte order. */
struct atos_sockaddr_in {
    uint16_t family; /* ATOS_AF_INET */
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};

/* The (single) network interface's configuration and counters. Addresses
 * in network byte order. */
struct atos_netinfo {
    uint8_t mac[6];
    uint16_t reserved;
    uint32_t addr, netmask, gateway, dns;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
};

#define ATOS_NAME_MAX 64 /* including the terminating NUL */

struct atos_dirent {
    char name[ATOS_NAME_MAX];
    uint32_t type;
    uint32_t reserved;
    uint64_t size;
};

struct atos_stat {
    uint32_t type;
    uint32_t reserved;
    uint64_t size;
};

#define EPERM         1
#define ENOENT        2
#define ESRCH         3
#define EIO           5
#define E2BIG         7
#define ENOEXEC       8
#define EBADF         9
#define ECHILD       10
#define ENOMEM       12
#define EFAULT       14
#define EEXIST       17
#define EBUSY        16
#define EXDEV        18
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39
#define ENODEV       19
#define EAGAIN       11
#define EPIPE        32
#define ENOTSOCK     88
#define EDESTADDRREQ 89
#define EMSGSIZE     90
#define EPROTOTYPE   91
#define EOPNOTSUPP   95
#define EADDRINUSE   98
#define ENETUNREACH 101
#define ECONNRESET  104
#define EISCONN     106
#define ENOTCONN    107
#define ETIMEDOUT   110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113

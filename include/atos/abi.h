/* The kernel <-> userspace ABI: syscall numbers, flag values, errno codes,
 * and the structs syscalls exchange. Included by both the kernel and the
 * userspace libc, so the two can't drift apart. Plain C, no kernel types. */
#pragma once
#include <stdint.h>

/* int 0x80: number in %rax, arguments in %rdi, %rsi, %rdx (SysV argument
 * order, so a future syscall/sysret path keeps the same layout), result in
 * %rax. Failures return a negative errno. */
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
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39

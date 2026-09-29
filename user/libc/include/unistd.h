#pragma once
#include <stddef.h>
#include <stdint.h>
#include <atos/abi.h>

typedef int64_t ssize_t;
typedef int64_t off_t;

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

/* Thin wrappers over the ATOS syscalls (include/atos/abi.h). On failure
 * they return -1 and set errno, like their POSIX namesakes. */
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
void *sbrk(intptr_t increment);
int sched_yield(void);

typedef int64_t pid_t;
pid_t fork(void);
/* Runs `path` with the NULL-terminated argv; returns only on failure. */
int execv(const char *path, char *const argv[]);
pid_t getpid(void);
int dup2(int oldfd, int newfd);
__attribute__((noreturn)) void _exit(int code);
/* ATOS_REBOOT_POWEROFF or ATOS_REBOOT_RESTART; returns only on failure. */
int reboot(int how);
unsigned sleep(unsigned seconds);

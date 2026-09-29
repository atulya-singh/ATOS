#include "syscall.h"
#include "uaccess.h"
#include "../acpi/acpi.h"
#include "../arch/x86_64/idt.h"
#include "../lib/kprintf.h"
#include "../fs/vfs.h"
#include "../proc/process.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../net/socket.h"
#include "../dev/timer.h"
#include "../sched/sched.h"

/* Cap on one read/write, so a single call can't keep a task in the kernel
 * for an unbounded time. Callers loop, as with any short read/write. */
#define IO_MAX (64 * 1024)
#define PATH_MAX_USER 256

static int64_t sys_write(int fd, uint64_t buf, uint64_t len) {
    struct file *f = fd_get(sched_current(), fd);
    if (!f) return -EBADF;
    if (len > IO_MAX) len = IO_MAX;
    /* Filesystems may copy straight from `buf`, so it's vetted here once
     * rather than in every driver. */
    if (!user_range_ok(buf, len, 0)) return -EFAULT;
    return vfs_write(f, (const void *)buf, len);
}

static int64_t sys_read(int fd, uint64_t buf, uint64_t len) {
    struct file *f = fd_get(sched_current(), fd);
    if (!f) return -EBADF;
    if (len > IO_MAX) len = IO_MAX;
    if (!user_range_ok(buf, len, 1)) return -EFAULT;
    return vfs_read(f, (void *)buf, len);
}

static int64_t sys_open(uint64_t upath, int flags) {
    char path[PATH_MAX_USER];
    int64_t err = strncpy_from_user(path, upath, sizeof(path));
    if (err < 0) return err;

    struct file *f;
    err = vfs_open(path, flags, &f);
    if (err) return err;
    int fd = fd_install(sched_current(), f);
    if (fd < 0) file_close(f);
    return fd;
}

static int64_t sys_seek(int fd, int64_t offset, int whence) {
    struct file *f = fd_get(sched_current(), fd);
    if (!f) return -EBADF;
    return vfs_seek(f, offset, whence);
}

static int64_t sys_readdir(int fd, uint64_t index, uint64_t uent) {
    struct file *f = fd_get(sched_current(), fd);
    if (!f) return -EBADF;
    struct atos_dirent ent;
    int err = vfs_readdir(f, index, &ent);
    if (err) return err;
    return copy_to_user(uent, &ent, sizeof(ent));
}

static int64_t sys_fstat(int fd, uint64_t ust) {
    struct file *f = fd_get(sched_current(), fd);
    if (!f) return -EBADF;
    struct atos_stat st;
    vfs_stat(f, &st);
    return copy_to_user(ust, &st, sizeof(st));
}

static int64_t sys_fork(struct registers *regs) {
    struct task *child = task_fork(regs);
    return child ? (int64_t)child->id : -ENOMEM;
}

/* Copies path and the NULL-terminated argv out of user memory up front:
 * exec destroys the address space they live in. */
static int64_t sys_exec(struct registers *regs, uint64_t upath, uint64_t uargv) {
    char path[PATH_MAX_USER];
    int64_t err = strncpy_from_user(path, upath, sizeof(path));
    if (err < 0) return err;

    char *strings = kmalloc(ARGS_MAX);
    if (!strings) return -ENOMEM;
    const char *argv[ARGV_MAX];
    int argc = 0;
    size_t used = 0;
    for (;;) {
        uint64_t uarg;
        err = copy_from_user(&uarg, uargv + (uint64_t)argc * 8, 8);
        if (err) goto out;
        if (!uarg) break;
        if (argc == ARGV_MAX - 1) { err = -E2BIG; goto out; }
        int64_t len = strncpy_from_user(strings + used, uarg, ARGS_MAX - used);
        if (len < 0) { err = len == -ENAMETOOLONG ? -E2BIG : len; goto out; }
        argv[argc++] = strings + used;
        used += (size_t)len + 1;
    }
    err = process_exec(regs, path, argc, argv);

out:
    kfree(strings);
    return err;
}

static int64_t sys_waitpid(int64_t pid, uint64_t ustatus) {
    int code;
    if (ustatus && !user_range_ok(ustatus, sizeof(int), 1)) return -EFAULT;
    int64_t id = task_wait(pid, &code);
    if (id > 0 && ustatus) copy_to_user(ustatus, &code, sizeof(code));
    return id;
}

static int64_t sys_reboot(uint64_t how) {
    if (how != ATOS_REBOOT_POWEROFF && how != ATOS_REBOOT_RESTART) return -EINVAL;
    /* Every FAT write goes straight to the disk, so there is nothing to
     * flush first. */
    kprintf("ATOS: %s\n", how == ATOS_REBOOT_POWEROFF ? "powering off" : "restarting");
    if (how == ATOS_REBOOT_POWEROFF) acpi_poweroff();
    acpi_reboot();
}

void syscall_handler(struct registers *regs) {
    /* Entered through an interrupt gate, so IF is clear. Syscalls can block
     * on disk I/O for a while, and every kernel structure they touch is
     * already safe against preemption, so let the timer back in. iretq
     * restores the caller's own flags on the way out. */
    asm volatile("sti");

    uint64_t a0 = regs->rdi, a1 = regs->rsi, a2 = regs->rdx, a3 = regs->r10;
    int64_t ret;
    switch (regs->rax) {
    case SYS_WRITE:   ret = sys_write((int)a0, a1, a2); break;
    case SYS_READ:    ret = sys_read((int)a0, a1, a2); break;
    case SYS_OPEN:    ret = sys_open(a0, (int)a1); break;
    case SYS_CLOSE:   ret = fd_close(sched_current(), (int)a0); break;
    case SYS_SEEK:    ret = sys_seek((int)a0, (int64_t)a1, (int)a2); break;
    case SYS_READDIR: ret = sys_readdir((int)a0, a1, a2); break;
    case SYS_FSTAT:   ret = sys_fstat((int)a0, a1); break;
    case SYS_BRK:     ret = (int64_t)process_brk(a0); break;
    case SYS_FORK:    ret = sys_fork(regs); break;
    case SYS_EXEC:    ret = sys_exec(regs, a0, a1); break;
    case SYS_WAITPID: ret = sys_waitpid((int64_t)a0, a1); break;
    case SYS_GETPID:  ret = (int64_t)sched_current()->id; break;
    case SYS_DUP2:    ret = fd_dup2(sched_current(), (int)a0, (int)a1); break;
    case SYS_REBOOT:  ret = sys_reboot(a0); break;
    case SYS_SOCKET:  ret = sys_socket((int)a0); break;
    case SYS_BIND:    ret = sys_bind((int)a0, a1); break;
    case SYS_CONNECT: ret = sys_connect((int)a0, a1); break;
    case SYS_LISTEN:  ret = sys_listen((int)a0, (int)a1); break;
    case SYS_ACCEPT:  ret = sys_accept((int)a0, a1); break;
    case SYS_SENDTO:  ret = sys_sendto((int)a0, a1, a2, a3); break;
    case SYS_RECVFROM: ret = sys_recvfrom((int)a0, a1, a2, a3); break;
    case SYS_SOCKOPT: ret = sys_sockopt((int)a0, (int)a1, a2); break;
    case SYS_NETINFO: ret = sys_netinfo(a0); break;
    case SYS_UPTIME:  ret = (int64_t)(timer_ticks() * (1000 / TIMER_HZ)); break;
    case SYS_SLEEP:   task_sleep((a0 * TIMER_HZ + 999) / 1000); ret = 0; break;
    case SYS_YIELD:   sched_yield(); ret = 0; break;
    case SYS_EXIT:    task_exit((int)a0);
    default:          ret = -ENOSYS; break;
    }
    regs->rax = (uint64_t)ret;
}

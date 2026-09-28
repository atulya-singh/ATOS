#include "syscall.h"
#include "uaccess.h"
#include "../arch/x86_64/idt.h"
#include "../fs/vfs.h"
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

void syscall_handler(struct registers *regs) {
    /* Entered through an interrupt gate, so IF is clear. Syscalls can block
     * on disk I/O for a while, and every kernel structure they touch is
     * already safe against preemption, so let the timer back in. iretq
     * restores the caller's own flags on the way out. */
    asm volatile("sti");

    uint64_t a0 = regs->rdi, a1 = regs->rsi, a2 = regs->rdx;
    int64_t ret;
    switch (regs->rax) {
    case SYS_WRITE:   ret = sys_write((int)a0, a1, a2); break;
    case SYS_READ:    ret = sys_read((int)a0, a1, a2); break;
    case SYS_OPEN:    ret = sys_open(a0, (int)a1); break;
    case SYS_CLOSE:   ret = fd_close(sched_current(), (int)a0); break;
    case SYS_SEEK:    ret = sys_seek((int)a0, (int64_t)a1, (int)a2); break;
    case SYS_READDIR: ret = sys_readdir((int)a0, a1, a2); break;
    case SYS_FSTAT:   ret = sys_fstat((int)a0, a1); break;
    case SYS_YIELD:   sched_yield(); ret = 0; break;
    case SYS_EXIT:    task_exit((int)a0);
    default:          ret = -ENOSYS; break;
    }
    regs->rax = (uint64_t)ret;
}

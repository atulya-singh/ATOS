#include "syscall.h"
#include "../arch/x86_64/idt.h"
#include "../dev/keyboard.h"
#include "../dev/console.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"

/* Cap on a single read/write, so one call can't hog the CPU with interrupts
 * off for an unbounded time (serial output is slow). Callers loop, as
 * they would with any short write. */
#define IO_MAX 4096

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1 && fd != 2) return -EBADF;
    if (len > IO_MAX) len = IO_MAX;
    /* Without this check a user pointer into the kernel half would get the
     * kernel to read its own memory on the caller's behalf, and an
     * unmapped one would page-fault inside the kernel. */
    if (!vmm_user_range_ok(sched_current()->cr3, buf, len, 0)) return -EFAULT;

    console_write((const char *)buf, len);
    return (int64_t)len;
}

/* Blocks until at least one key is queued, then returns whatever else is
 * already waiting (up to len), like a raw-mode tty read. Line editing is
 * the reader's job. */
static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 0) return -EBADF;
    if (len == 0) return 0;
    if (len > IO_MAX) len = IO_MAX;
    if (!vmm_user_range_ok(sched_current()->cr3, buf, len, 1)) return -EFAULT;

    char *p = (char *)buf;
    p[0] = keyboard_getc();
    uint64_t n = 1;
    int c;
    while (n < len && (c = keyboard_try_getc()) >= 0) p[n++] = (char)c;
    return (int64_t)n;
}

void syscall_handler(struct registers *regs) {
    int64_t ret;
    switch (regs->rax) {
    case SYS_WRITE:
        ret = sys_write(regs->rdi, regs->rsi, regs->rdx);
        break;
    case SYS_EXIT:
        task_exit((int)regs->rdi);
    case SYS_READ:
        ret = sys_read(regs->rdi, regs->rsi, regs->rdx);
        break;
    case SYS_YIELD:
        sched_yield();
        ret = 0;
        break;
    default:
        ret = -ENOSYS;
        break;
    }
    regs->rax = (uint64_t)ret;
}

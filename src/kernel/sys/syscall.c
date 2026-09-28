#include "syscall.h"
#include "../arch/x86_64/idt.h"
#include "../dev/serial.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"

/* Cap on a single write, so one call can't hog the CPU with interrupts
 * off for an unbounded time (serial output is slow). Callers loop, as
 * they would with any short write. */
#define WRITE_MAX 4096

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1 && fd != 2) return -EBADF;
    if (len > WRITE_MAX) len = WRITE_MAX;
    /* Without this check a user pointer into the kernel half would get the
     * kernel to read its own memory on the caller's behalf, and an
     * unmapped one would page-fault inside the kernel. */
    if (!vmm_user_range_ok(sched_current()->cr3, buf, len, 0)) return -EFAULT;

    const char *p = (const char *)buf;
    for (uint64_t i = 0; i < len; i++) serial_putc(p[i]);
    return (int64_t)len;
}

void syscall_handler(struct registers *regs) {
    int64_t ret;
    switch (regs->rax) {
    case SYS_WRITE:
        ret = sys_write(regs->rdi, regs->rsi, regs->rdx);
        break;
    case SYS_EXIT:
        task_exit((int)regs->rdi);
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

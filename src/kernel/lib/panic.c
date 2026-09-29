#include "panic.h"
#include "kprintf.h"
#include "ksyms.h"
#include "spinlock.h"
#include "../arch/x86_64/idt.h"
#include "../arch/x86_64/percpu.h"
#include "../arch/x86_64/smp.h"
#include "../dev/console.h"
#include "../dev/fbcon.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include <stdarg.h>

#define BACKTRACE_MAX_FRAMES 32
#define KERNEL_HALF          0xFFFF800000000000ULL
#define KSTACK_REGION        0xFFFFB00000000000ULL
#define KSTACK_REGION_END    0xFFFFB00100000000ULL

static int panicking;

static int kernel_mapped(uint64_t addr) {
    uint64_t phys;
    return addr >= KERNEL_HALF && vmm_translate(addr, &phys);
}

static void print_frame(uint64_t addr) {
    uint64_t off;
    const char *name = ksym_lookup(addr, &off);
    if (name) kprintf("  [%#016lx] %s+%#lx\n", addr, name, off);
    else kprintf("  [%#016lx] ?\n", addr);
}

static void print_repeats(uint64_t repeats) {
    if (repeats) kprintf("  ... same frame %lu more time(s)\n", repeats);
}

void backtrace_print(uint64_t rip, uint64_t rbp) {
    kprintf("backtrace:\n");
    print_frame(rip);
    /* Deep recursion would bury everything else, so identical consecutive
     * frames collapse into a count (and don't use up the frame budget). */
    uint64_t last = 0, repeats = 0;
    for (int printed = 0; printed < BACKTRACE_MAX_FRAMES;) {
        /* A frame is [saved rbp][return address]; both must be readable. */
        if (rbp == 0 || (rbp & 7) || !kernel_mapped(rbp) || !kernel_mapped(rbp + 15)) break;
        const uint64_t *frame = (const uint64_t *)rbp;
        uint64_t ret = frame[1];
        uint64_t off;
        if (!ksym_lookup(ret, &off)) break; /* left kernel code (e.g. a user frame) */
        if (ret == last) {
            repeats++;
        } else {
            print_repeats(repeats);
            repeats = 0;
            print_frame(ret);
            last = ret;
            printed++;
        }
        if (frame[0] <= rbp) break; /* callers live higher up the stack */
        rbp = frame[0];
    }
    print_repeats(repeats);
}

int panic_in_progress(void) {
    return __atomic_load_n(&panicking, __ATOMIC_ACQUIRE) != 0;
}

/* Once a panic starts, nothing else may run or print: interrupts off here,
 * every other CPU stopped with an NMI, and a second panic (say, a fault
 * while printing, or a racing panic on another CPU) just stops. */
static void panic_begin(void) {
    asm volatile("cli");
    int me = (int)this_cpu()->index + 1, none = 0;
    if (!__atomic_compare_exchange_n(&panicking, &none, me, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (panicking == me) {
            console_lock.locked = 0;
            kprintf("\n--- nested panic, halting ---\n");
        }
        for (;;) asm volatile("cli; hlt");
    }
    smp_halt_others();
    /* A stopped CPU may have died holding the console lock. */
    console_lock.owner = 0;
    console_lock.locked = 0;
    fbcon_panic_screen();
    kprintf("\n*** ATOS KERNEL PANIC ***\n");
}

void spinlock_recursion(struct spinlock *l) {
    panic("spinlock %p taken twice by CPU %u", (void *)l, this_cpu()->index);
}

static __attribute__((noreturn)) void panic_end(void) {
    struct task *t = sched_current();
    kprintf("cpu: %u of %u\n", this_cpu()->index, cpu_count);
    if (t) kprintf("task: %lu (%s)\n", t->id, t->name);
    kprintf("--- system halted ---\n");
    for (;;) asm volatile("cli; hlt");
}

void panic(const char *fmt, ...) {
    panic_begin();
    va_list args;
    va_start(args, fmt);
    kvprintf(fmt, args);
    va_end(args);
    kprintf("\n");
    uint64_t rbp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    backtrace_print((uint64_t)__builtin_return_address(0), *(const uint64_t *)rbp);
    panic_end();
}

void panic_exception(const struct registers *regs, const char *what, uint64_t cr2) {
    panic_begin();
    kprintf("unhandled exception %lu (%s), error code %#lx\n", regs->int_no, what, regs->err_code);
    if (regs->int_no == 14) kprintf("faulting address: %#016lx\n", cr2);
    /* A #DF whose stack pointer sits just below a task stack means the
     * task ran off the end of its stack into the guard page. */
    if (regs->int_no == 8 && regs->rsp >= KSTACK_REGION && regs->rsp < KSTACK_REGION_END &&
        !kernel_mapped(regs->rsp - 8)) {
        kprintf("likely cause: kernel stack overflow (rsp in a guard page)\n");
    }
    kprintf("rip=%#016lx cs=%#lx rflags=%#lx rsp=%#016lx ss=%#lx\n",
            regs->rip, regs->cs, regs->rflags, regs->rsp, regs->ss);
    kprintf("rax=%#016lx rbx=%#016lx rcx=%#016lx rdx=%#016lx\n",
            regs->rax, regs->rbx, regs->rcx, regs->rdx);
    kprintf("rsi=%#016lx rdi=%#016lx rbp=%#016lx\n", regs->rsi, regs->rdi, regs->rbp);
    kprintf("r8 =%#016lx r9 =%#016lx r10=%#016lx r11=%#016lx\n",
            regs->r8, regs->r9, regs->r10, regs->r11);
    kprintf("r12=%#016lx r13=%#016lx r14=%#016lx r15=%#016lx\n",
            regs->r12, regs->r13, regs->r14, regs->r15);
    uint64_t cr0, cr3, cr4;
    asm volatile("mov %%cr0, %0; mov %%cr3, %1; mov %%cr4, %2" : "=r"(cr0), "=r"(cr3), "=r"(cr4));
    kprintf("cr0=%#lx cr2=%#lx cr3=%#lx cr4=%#lx\n", cr0, cr2, cr3, cr4);
    backtrace_print(regs->rip, regs->rbp);
    panic_end();
}

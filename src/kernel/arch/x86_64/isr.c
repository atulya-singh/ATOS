#include "idt.h"
#include "../../dev/pic.h"
#include "../../dev/pit.h"
#include "../../lib/kprintf.h"
#include "../../sched/sched.h"

static const char *const exception_names[32] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check", "SIMD Floating-Point",
    "Virtualization", "Control Protection", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Hypervisor Injection", "VMM Communication",
    "Security", "Reserved",
};

static void print_pf_decode(uint64_t err_code, uint64_t cr2) {
    /* #PF error code bits: every kernel-mode fault today is a genuine bug
     * (no COW/demand-paging path exists yet), so decoding these is what
     * makes *which* bug it is obvious. */
    kprintf(" cr2=%#lx [%s, %s, %s%s%s]", cr2,
            (err_code & 0x1) ? "protection-violation" : "non-present",
            (err_code & 0x2) ? "write" : "read",
            (err_code & 0x4) ? "user-mode" : "supervisor-mode",
            (err_code & 0x8) ? ", reserved-bit-violation" : "",
            (err_code & 0x10) ? ", instruction-fetch" : "");
}

/* Every CPU exception (vectors 0-31) lands here. A fault raised in ring 3
 * is the user program's problem: only that task dies, and the rest of the
 * system carries on. A fault in the kernel has no recovery path, so we dump
 * the full context and halt; that is what makes a triple fault diagnosable
 * instead of a silent reboot loop. */
void isr_handler(struct registers *regs) {
    uint64_t cr2 = 0;
    if (regs->int_no == 14) {
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
    }

    if ((regs->cs & 3) == 3) {
        struct task *t = sched_current();
        kprintf("ATOS: task %lu (%s) killed: %s at rip=%#lx",
                t->id, t->name, exception_names[regs->int_no], regs->rip);
        if (regs->int_no == 14) print_pf_decode(regs->err_code, cr2);
        kprintf("\n");
        task_exit(128 + (int)regs->int_no); /* shell-style "killed by" code */
    }

    kprintf("\n--- unhandled exception %lu (%s) ---\n", regs->int_no,
            exception_names[regs->int_no]);
    kprintf("error_code=%#lx", regs->err_code);
    if (regs->int_no == 14) print_pf_decode(regs->err_code, cr2);
    kprintf("\nrip=%#016lx cs=%#lx rflags=%#lx rsp=%#016lx\n",
            regs->rip, regs->cs, regs->rflags, regs->rsp);
    kprintf("rax=%#016lx rbx=%#016lx rcx=%#016lx rdx=%#016lx\n",
            regs->rax, regs->rbx, regs->rcx, regs->rdx);
    kprintf("rsi=%#016lx rdi=%#016lx rbp=%#016lx\n", regs->rsi, regs->rdi, regs->rbp);
    kprintf("r8=%#016lx r9=%#016lx r10=%#016lx r11=%#016lx\n",
            regs->r8, regs->r9, regs->r10, regs->r11);
    kprintf("r12=%#016lx r13=%#016lx r14=%#016lx r15=%#016lx\n",
            regs->r12, regs->r13, regs->r14, regs->r15);
    kprintf("--- system halted ---\n");

    for (;;) asm volatile("cli; hlt");
}

static void (*irq_handlers[16])(void);

void irq_install_handler(uint8_t irq, void (*handler)(void)) {
    irq_handlers[irq] = handler;
    if (irq >= 8) pic_clear_mask(2); /* slave PIC reaches the CPU via IRQ2 */
    pic_clear_mask(irq);
}

void irq_handler(struct registers *regs) {
    uint64_t irq = regs->int_no - 32;

    if (irq == 0) pit_tick();
    else if (irq_handlers[irq]) irq_handlers[irq]();

    /* EOI before any task switch: the task we switch to may not come back
     * through here for a long time, and until the PIC sees EOI it holds
     * off every further timer interrupt. */
    pic_send_eoi((uint8_t)irq);

    if (irq == 0) sched_tick();
}

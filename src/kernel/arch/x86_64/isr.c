#include "idt.h"
#include "../../dev/pic.h"
#include "../../dev/pit.h"
#include "../../lib/kprintf.h"

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

/* Every CPU exception (vectors 0-31) lands here. There's no recovery path
 * yet -- that arrives with demand paging / per-process fault isolation in
 * later phases -- so for now we dump full context and halt, which is what
 * makes a triple fault diagnosable instead of a silent reboot loop. */
void isr_handler(struct registers *regs) {
    uint64_t cr2 = 0;
    if (regs->int_no == 14) {
        asm volatile("mov %%cr2, %0" : "=r"(cr2));
    }

    kprintf("\n--- unhandled exception %lu (%s) ---\n", regs->int_no,
            exception_names[regs->int_no]);
    kprintf("error_code=%#lx", regs->err_code);
    if (regs->int_no == 14) {
        /* #PF error code bits: every fault today is a genuine bug (no
         * COW/demand-paging path exists until processes do in Phase 3+),
         * so decoding these is what makes *which* bug it is obvious. */
        kprintf(" cr2=%#lx [%s, %s, %s%s%s]", cr2,
                (regs->err_code & 0x1) ? "protection-violation" : "non-present",
                (regs->err_code & 0x2) ? "write" : "read",
                (regs->err_code & 0x4) ? "user-mode" : "supervisor-mode",
                (regs->err_code & 0x8) ? ", reserved-bit-violation" : "",
                (regs->err_code & 0x10) ? ", instruction-fetch" : "");
    }
    kprintf("\nrip=%#016lx cs=%#lx rflags=%#lx\n", regs->rip, regs->cs, regs->rflags);
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

void irq_handler(struct registers *regs) {
    uint64_t irq = regs->int_no - 32;

    if (irq == 0) pit_tick();

    pic_send_eoi((uint8_t)irq);
}

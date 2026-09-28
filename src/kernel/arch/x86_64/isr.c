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

    kprintf("\n--- unhandled exception %llu (%s) ---\n", regs->int_no,
            exception_names[regs->int_no]);
    kprintf("error_code=%#llx", regs->err_code);
    if (regs->int_no == 14) kprintf(" cr2=%#llx", cr2);
    kprintf("\nrip=%#016llx cs=%#llx rflags=%#llx\n", regs->rip, regs->cs, regs->rflags);
    kprintf("rax=%#016llx rbx=%#016llx rcx=%#016llx rdx=%#016llx\n",
            regs->rax, regs->rbx, regs->rcx, regs->rdx);
    kprintf("rsi=%#016llx rdi=%#016llx rbp=%#016llx\n", regs->rsi, regs->rdi, regs->rbp);
    kprintf("r8=%#016llx r9=%#016llx r10=%#016llx r11=%#016llx\n",
            regs->r8, regs->r9, regs->r10, regs->r11);
    kprintf("r12=%#016llx r13=%#016llx r14=%#016llx r15=%#016llx\n",
            regs->r12, regs->r13, regs->r14, regs->r15);
    kprintf("--- system halted ---\n");

    for (;;) asm volatile("cli; hlt");
}

void irq_handler(struct registers *regs) {
    uint64_t irq = regs->int_no - 32;

    if (irq == 0) pit_tick();

    pic_send_eoi((uint8_t)irq);
}

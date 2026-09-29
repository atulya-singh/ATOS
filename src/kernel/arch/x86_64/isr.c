#include "idt.h"
#include "ioapic.h"
#include "lapic.h"
#include "percpu.h"
#include "smp.h"
#include "../../dev/timer.h"
#include "../../lib/kprintf.h"
#include "../../lib/panic.h"
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
 * system carries on. A fault in the kernel has no recovery path: panic with
 * the full context and a backtrace. */
void isr_handler(struct registers *regs) {
    /* The only NMIs ATOS sends are panic()'s "stop now" to the other CPUs. */
    if (regs->int_no == 2 && panic_in_progress()) {
        for (;;) asm volatile("cli; hlt");
    }

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

    panic_exception(regs, exception_names[regs->int_no], cr2);
}

static void (*irq_handlers[16])(void);

void irq_install_handler(uint8_t irq, void (*handler)(void)) {
    irq_handlers[irq] = handler;
    ioapic_route_isa_irq(irq, (uint8_t)(VEC_IRQ_BASE + irq), cpus[0].apic_id);
}

/* Every hardware interrupt and IPI lands here (see idt.h for vectors). */
void irq_handler(struct registers *regs) {
    uint64_t vector = regs->int_no;

    if (vector == VEC_TIMER) {
        /* EOI before any task switch: the task we switch to may not come
         * back through here for a long time, and until the LAPIC sees EOI
         * it holds off every further timer interrupt. */
        lapic_eoi();
        timer_tick();
        sched_tick();
        return;
    }
    if (vector == VEC_TLB_SHOOTDOWN) {
        tlb_shootdown_ipi();
    } else {
        uint64_t irq = vector - VEC_IRQ_BASE;
        if (irq < 16 && irq_handlers[irq]) irq_handlers[irq]();
    }
    lapic_eoi();
}

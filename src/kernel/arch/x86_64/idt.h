#pragma once
#include <stdint.h>

/* Layout matches exactly what isr_stubs.S pushes onto the stack, read back
 * as a struct pointer passed in %rdi. See the push/pop order in that file
 * before changing either side of this contract. */
struct registers {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags;
    uint64_t rsp, ss; /* long mode always pushes these, even for ring0->ring0 */
} __attribute__((packed));

/* Interrupt vectors. 32-47 are "IRQ" stubs: 32 is each CPU's LAPIC timer,
 * and 32 + n is ISA IRQ n (1-15) once routed through the I/O APIC. */
#define VEC_IRQ_BASE      32
#define VEC_TIMER         32
#define VEC_TLB_SHOOTDOWN 0xF0
#define VEC_SPURIOUS      0xFF
#define SYSCALL_VECTOR    0x80

/* Builds the IDT and loads it on the calling (bootstrap) CPU. */
void idt_init(void);
/* Loads the already-built IDT; for application processors. */
void idt_load(void);

/* Routes ISA IRQ `irq` (1-15) to `handler`, via the I/O APIC to the
 * bootstrap CPU. The handler runs in interrupt context before EOI; keep
 * it short and don't block. */
void irq_install_handler(uint8_t irq, void (*handler)(void));

#include "idt.h"
#include "gdt.h"
#include "../../dev/pic.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtr;

extern void idt_flush(uint64_t idtr_addr);

#define ISR_LIST(X) \
    X(0)  X(1)  X(2)  X(3)  X(4)  X(5)  X(6)  X(7)  X(8)  X(9)  \
    X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19) \
    X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) \
    X(30) X(31)

#define IRQ_LIST(X) \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)

#define DECLARE_ISR(n) extern void isr##n(void);
ISR_LIST(DECLARE_ISR)
#undef DECLARE_ISR

#define DECLARE_IRQ(n) extern void irq##n(void);
IRQ_LIST(DECLARE_IRQ)
#undef DECLARE_IRQ

static void idt_set_gate(uint8_t vector, void (*handler)(void), uint8_t ist) {
    uint64_t addr = (uint64_t)handler;
    idt[vector].offset_low  = addr & 0xFFFF;
    idt[vector].selector    = GDT_KERNEL_CODE;
    idt[vector].ist         = ist;
    idt[vector].type_attr   = 0x8E; /* present, ring0, 64-bit interrupt gate */
    idt[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].zero        = 0;
}

void idt_init(void) {
#define SET_ISR(n) idt_set_gate(n, isr##n, (n) == 8 ? 1 : 0);
    ISR_LIST(SET_ISR)
#undef SET_ISR

#define SET_IRQ(n) idt_set_gate(32 + (n), irq##n, 0);
    IRQ_LIST(SET_IRQ)
#undef SET_IRQ

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64_t)&idt;
    idt_flush((uint64_t)&idtr);

    pic_remap();
    /* Mask every IRQ line until its driver is ready; only the timer (IRQ0)
     * is wired up so far. */
    for (int i = 0; i < 16; i++) pic_set_mask((uint8_t)i);
    pic_clear_mask(0);
}

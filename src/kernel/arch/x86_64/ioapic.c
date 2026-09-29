#include "ioapic.h"
#include "../../acpi/acpi.h"
#include "../../lib/kprintf.h"
#include "../../lib/spinlock.h"
#include "../../mm/vmm.h"

#define IOREGSEL 0 /* register index, as a 32-bit word offset */
#define IOWIN    4 /* data window at byte offset 0x10 */

#define REG_VERSION 0x01
#define REG_REDIR(n) (0x10 + 2 * (n))

#define REDIR_ACTIVE_LOW (1u << 13)
#define REDIR_LEVEL      (1u << 15)
#define REDIR_MASKED     (1u << 16)

struct ioapic {
    volatile uint32_t *regs;
    uint32_t gsi_base;
    uint32_t inputs;
};

static struct ioapic ioapics[ACPI_MAX_IOAPICS];
static unsigned ioapic_count;
static struct spinlock lock; /* IOREGSEL/IOWIN is a shared register window */

static uint32_t rd(struct ioapic *io, uint32_t reg) {
    io->regs[IOREGSEL] = reg;
    return io->regs[IOWIN];
}

static void wr(struct ioapic *io, uint32_t reg, uint32_t v) {
    io->regs[IOREGSEL] = reg;
    io->regs[IOWIN] = v;
}

void ioapic_init(void) {
    const struct acpi_madt_info *madt = acpi_madt();
    for (unsigned i = 0; madt && i < madt->ioapic_count; i++) {
        struct ioapic *io = &ioapics[ioapic_count++];
        io->regs = vmm_map_phys(madt->ioapics[i].addr, 0x20, VMM_NOCACHE);
        io->gsi_base = madt->ioapics[i].gsi_base;
        io->inputs = ((rd(io, REG_VERSION) >> 16) & 0xFF) + 1;
        for (uint32_t n = 0; n < io->inputs; n++) {
            wr(io, REG_REDIR(n) + 1, 0);
            wr(io, REG_REDIR(n), REDIR_MASKED);
        }
        kprintf("ATOS: ioapic: id %u, GSIs %u-%u\n", madt->ioapics[i].id, io->gsi_base,
                io->gsi_base + io->inputs - 1);
    }
}

void ioapic_route_isa_irq(uint8_t irq, uint8_t vector, uint32_t apic_id) {
    uint16_t flags;
    uint32_t gsi = acpi_isa_irq_to_gsi(irq, &flags);
    uint32_t low = vector; /* fixed delivery, physical destination */
    /* ISA defaults are edge-triggered, active high; overrides may differ. */
    if ((flags & ACPI_INTI_POLARITY_MASK) == ACPI_INTI_ACTIVE_LOW) low |= REDIR_ACTIVE_LOW;
    if ((flags & ACPI_INTI_TRIGGER_MASK) == ACPI_INTI_LEVEL) low |= REDIR_LEVEL;

    for (unsigned i = 0; i < ioapic_count; i++) {
        struct ioapic *io = &ioapics[i];
        if (gsi < io->gsi_base || gsi >= io->gsi_base + io->inputs) continue;
        uint32_t n = gsi - io->gsi_base;
        uint64_t f = spin_lock_irqsave(&lock);
        wr(io, REG_REDIR(n) + 1, apic_id << 24);
        wr(io, REG_REDIR(n), low); /* unmasked from here on */
        spin_unlock_irqrestore(&lock, f);
        return;
    }
    kprintf("ATOS: ioapic: no I/O APIC handles GSI %u (IRQ %u)\n", gsi, irq);
}

#pragma once
#include <stdint.h>

/* I/O APIC: routes device interrupt lines (global system interrupts) to
 * a vector on a chosen CPU. Replaces the legacy 8259 PICs. */

/* Maps every I/O APIC the MADT lists and masks all of their inputs. */
void ioapic_init(void);

/* Routes ISA IRQ `irq` -- through any MADT source override, with its
 * polarity and trigger mode -- to `vector` on the CPU with `apic_id`. */
void ioapic_route_isa_irq(uint8_t irq, uint8_t vector, uint32_t apic_id);

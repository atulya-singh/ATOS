#pragma once
#include <stdint.h>

/* Local APIC: each CPU's own interrupt controller, timer, and IPI sender
 * (xAPIC mode, memory-mapped). */

/* Maps the LAPIC registers (address from the ACPI MADT); BSP, once. */
void lapic_init(uint64_t phys_addr);

/* Enables the calling CPU's LAPIC and routes spurious interrupts to
 * VEC_SPURIOUS. Every CPU runs this for itself. */
void lapic_enable_cpu(void);

uint32_t lapic_id(void);
void lapic_eoi(void);

/* Measures the LAPIC timer's rate against the PIT (BSP, once, before any
 * CPU starts its timer), then starts the calling CPU's periodic timer at
 * `hz` on VEC_TIMER. */
void lapic_timer_calibrate(unsigned hz);
void lapic_timer_start(void);

/* Inter-processor interrupts. */
void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void lapic_broadcast_ipi(uint8_t vector); /* every CPU but the caller */
void lapic_broadcast_nmi(void);           /* every CPU but the caller */

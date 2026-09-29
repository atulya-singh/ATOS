#pragma once
#include <stdint.h>

/* Brings up every application processor Limine found: each gets a GDT,
 * TSS, idle task, LAPIC timer, and GS-based struct cpu, then joins the
 * scheduler. Call on the BSP once the scheduler exists and the LAPIC
 * timer is calibrated; returns once every AP is online. */
void smp_init(void);

/* Invalidates the kernel mappings of [start, start+len) on every other
 * online CPU (the caller does its own invlpg), and waits until they have.
 * Must be called with interrupts enabled and no spinlock held: the other
 * CPUs only answer once they can take the IPI. */
void tlb_shootdown(uint64_t start, uint64_t len);

/* The TLB shootdown IPI's handler. */
void tlb_shootdown_ipi(void);

/* Stops every other CPU (with an NMI), for panic. */
void smp_halt_others(void);

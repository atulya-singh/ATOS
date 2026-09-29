#pragma once
#include <stddef.h>
#include <stdint.h>
#include "gdt.h"

#define MAX_CPUS 16

struct task;

/* Everything one CPU owns. Each CPU's GS base points at its own struct
 * (user mode gets its own GS base, swapped in and out with swapgs at every
 * ring-3 boundary; see isr_stubs.S), so this_cpu() is a single load. */
struct cpu {
    struct cpu *self;          /* must stay first: this_cpu() reads %gs:0 */
    struct task *current;      /* must stay second: see sched_current() */
    uint32_t index;            /* 0 = the bootstrap processor */
    uint32_t apic_id;
    struct task *idle;         /* runs when nothing else is runnable here */
    struct task *switch_prev;  /* the task just switched away from; see schedule() */
    volatile int online;
    uint64_t boot_stack_top;   /* an AP's first kernel stack (its idle task's) */
    uint64_t gdt[7];           /* 5 segments + this CPU's TSS descriptor (2 slots) */
    struct tss tss;
};

extern struct cpu cpus[MAX_CPUS];
extern unsigned cpu_count; /* CPUs brought up so far (the BSP counts) */

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    asm volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

/* Points GS at `c` (and clears the user-side KERNEL_GS_BASE). Must run
 * after any reload of %gs itself, which would reset the base. */
void percpu_set(struct cpu *c);

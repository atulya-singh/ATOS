#pragma once
#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct cpu;

/* Loads a GDT and TSS for `c`, which each CPU needs its own copy of (a
 * TSS holds per-CPU stack pointers, and its descriptor is marked busy
 * once loaded). `df_stack_top` is the top of a stack reserved for double
 * faults. Reloads every segment register, including %gs, so GS-based
 * per-CPU data must be (re)set afterwards. */
void gdt_init_cpu(struct cpu *c, uint64_t df_stack_top);

/* The BSP's gdt_init_cpu, with a statically allocated double-fault stack. */
void gdt_init(void);

/* The stack the CPU switches to when an interrupt or int 0x80 arrives while
 * in ring 3. The scheduler points it at the incoming task's kernel stack
 * on every switch, so user-mode traps always land on a known-good stack. */
void tss_set_rsp0(uint64_t rsp0);

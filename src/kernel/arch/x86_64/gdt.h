#pragma once
#include <stdint.h>

#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_CODE   0x18
#define GDT_USER_DATA   0x20
#define GDT_TSS         0x28

void gdt_init(void);

/* The stack the CPU switches to when an interrupt or int 0x80 arrives while
 * in ring 3. The scheduler points it at the incoming task's kernel stack
 * on every switch, so user-mode traps always land on a known-good stack. */
void tss_set_rsp0(uint64_t rsp0);

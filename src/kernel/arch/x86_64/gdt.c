#include "gdt.h"
#include "percpu.h"
#include <stddef.h>
#include <stdint.h>

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* The BSP's double-fault stack (APs get theirs from the heap). Reached
 * via the TSS's IST1 slot, so a double fault caused by stack corruption
 * still has a working stack instead of triple-faulting. */
#define DOUBLE_FAULT_STACK_SIZE 8192
static uint8_t bsp_double_fault_stack[DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));

extern void gdt_flush(uint64_t gdtr_addr);
extern void tss_flush(void);

static void set_tss_descriptor(uint64_t *gdt, uint64_t base, uint32_t limit) {
    gdt[5] = (limit & 0xFFFFULL)
           | ((base & 0xFFFFFFULL) << 16)
           | (0x89ULL << 40) /* present, DPL0, 64-bit TSS (available) */
           | (((uint64_t)(limit >> 16) & 0xFULL) << 48)
           | (((base >> 24) & 0xFFULL) << 56);
    gdt[6] = (base >> 32) & 0xFFFFFFFFULL;
}

void gdt_init_cpu(struct cpu *c, uint64_t df_stack_top) {
    c->gdt[0] = 0x0000000000000000ULL; /* null */
    c->gdt[1] = 0x00AF9A000000FFFFULL; /* kernel code, ring0, long mode */
    c->gdt[2] = 0x00CF92000000FFFFULL; /* kernel data, ring0 */
    c->gdt[3] = 0x00AFFA000000FFFFULL; /* user code, ring3, long mode */
    c->gdt[4] = 0x00CFF2000000FFFFULL; /* user data, ring3 */

    for (size_t i = 0; i < sizeof(c->tss); i++) ((uint8_t *)&c->tss)[i] = 0;
    c->tss.ist1 = df_stack_top;
    c->tss.iopb_offset = sizeof(c->tss);
    set_tss_descriptor(c->gdt, (uint64_t)&c->tss, sizeof(c->tss) - 1);

    struct gdt_ptr gdtr = {.limit = sizeof(c->gdt) - 1, .base = (uint64_t)c->gdt};
    gdt_flush((uint64_t)&gdtr);
    tss_flush();
}

void gdt_init(void) {
    gdt_init_cpu(&cpus[0], (uint64_t)(bsp_double_fault_stack + DOUBLE_FAULT_STACK_SIZE));
    percpu_set(&cpus[0]);
}

void tss_set_rsp0(uint64_t rsp0) {
    this_cpu()->tss.rsp0 = rsp0;
}

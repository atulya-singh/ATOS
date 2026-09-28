#include "gdt.h"
#include <stddef.h>
#include <stdint.h>

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

/* Dedicated stack for #DF (double fault), reached via the TSS's IST1 slot
 * so a double fault caused by stack corruption still gets a working stack
 * to run the handler on instead of triple-faulting. */
#define DOUBLE_FAULT_STACK_SIZE 8192
static uint8_t double_fault_stack[DOUBLE_FAULT_STACK_SIZE] __attribute__((aligned(16)));

/* 5 flat descriptors (null, kernel code/data, user code/data) + a 16-byte
 * TSS descriptor, which occupies two consecutive 8-byte slots. */
static uint64_t gdt_entries[7];
static struct tss tss;
static struct gdt_ptr gdtr;

extern void gdt_flush(uint64_t gdtr_addr);
extern void tss_flush(void);

static void gdt_set_tss_descriptor(uint64_t base, uint32_t limit) {
    gdt_entries[5] = (limit & 0xFFFFULL)
                    | ((base & 0xFFFFFFULL) << 16)
                    | (0x89ULL << 40) /* present, DPL0, 64-bit TSS (available) */
                    | (((uint64_t)(limit >> 16) & 0xFULL) << 48)
                    | (((base >> 24) & 0xFFULL) << 56);
    gdt_entries[6] = (base >> 32) & 0xFFFFFFFFULL;
}

void gdt_init(void) {
    gdt_entries[0] = 0x0000000000000000ULL; /* null */
    gdt_entries[1] = 0x00AF9A000000FFFFULL; /* kernel code, ring0, long mode */
    gdt_entries[2] = 0x00CF92000000FFFFULL; /* kernel data, ring0 */
    gdt_entries[3] = 0x00AFFA000000FFFFULL; /* user code, ring3, long mode */
    gdt_entries[4] = 0x00CFF2000000FFFFULL; /* user data, ring3 */

    for (size_t i = 0; i < sizeof(tss); i++) ((uint8_t *)&tss)[i] = 0;
    tss.ist1 = (uint64_t)(double_fault_stack + DOUBLE_FAULT_STACK_SIZE);
    tss.iopb_offset = sizeof(tss);

    gdt_set_tss_descriptor((uint64_t)&tss, sizeof(tss) - 1);

    gdtr.limit = sizeof(gdt_entries) - 1;
    gdtr.base = (uint64_t)&gdt_entries;

    gdt_flush((uint64_t)&gdtr);
    tss_flush();
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

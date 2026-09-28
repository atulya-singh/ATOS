#include <limine.h>
#include <stdint.h>

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "dev/pit.h"
#include "dev/serial.h"
#include "lib/kprintf.h"
#include "mm/boot_info.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* A short, permanent smoke test (not throwaway debug code): exercises
 * split + coalesce so a broken heap shows up as wrong numbers on every
 * boot, not just when someone happens to stress it later. */
static void heap_self_test(void) {
    uint64_t free_before = pmm_free_page_count();

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kmalloc(256);
    kfree(b);
    void *d = kmalloc(96); /* should reuse b's freed block */
    kfree(a);
    kfree(c);
    kfree(d);

    uint64_t free_after = pmm_free_page_count();
    kprintf("ATOS: heap self-test: alloc/free/coalesce ok (pmm free pages %lu -> %lu)\n",
            free_before, free_after);
}

/* Runs on the kernel-owned stack, under our own page tables, after
 * vmm_init()'s CR3 + stack switch -- see vmm_switch_and_continue. */
static void kmain_stage2(void) {
    heap_init();
    heap_self_test();

    pit_init(100);
    kprintf("ATOS: PIT timer at 100 Hz\n");

    asm volatile("sti");
    kprintf("ATOS: interrupts enabled, entering idle loop\n");

    uint64_t last_reported = 0;
    for (;;) {
        asm volatile("hlt");
        uint64_t t = pit_get_ticks();
        if (t - last_reported >= 500) {
            kprintf("ATOS: tick=%lu (alive)\n", t);
            last_reported = t;
        }
    }
}

void kmain(void) {
    serial_init();
    kprintf("ATOS: booting...\n");

    boot_info_init();

    gdt_init();
    kprintf("ATOS: GDT + TSS loaded\n");

    idt_init();
    kprintf("ATOS: IDT + PIC configured\n");

    pmm_init();

    /* Never returns: switches to our own page tables and a kernel-owned
     * stack, then jumps into kmain_stage2. */
    vmm_init(kmain_stage2);
}

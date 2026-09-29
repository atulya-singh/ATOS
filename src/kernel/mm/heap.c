#include "heap.h"
#include "heap_core.h"
#include "pmm.h"
#include "vmm.h"
#include "../arch/x86_64/cpu.h"
#include "../lib/kprintf.h"

/* Far above the HHDM range (which only spans actual installed RAM) and the
 * kernel image, so it can't collide with either. */
#define KHEAP_BASE 0xFFFFA00000000000ULL
/* Fixed-size: simple, and it keeps the boot-time leak checks exact (a
 * growing heap would shift the free-page count mid-test). 4 MiB covers task
 * structs, open files, and filesystem metadata comfortably for now. */
#define KHEAP_SIZE (4 * 1024 * 1024ULL)

static struct heap_arena arena;

void heap_init(void) {
    uint64_t pages = KHEAP_SIZE / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        vmm_map(KHEAP_BASE + i * PAGE_SIZE, phys, VMM_PRESENT | VMM_WRITABLE | VMM_NX);
    }
    heap_arena_init(&arena, (void *)KHEAP_BASE, KHEAP_SIZE);
    kprintf("ATOS: kernel heap: %lu KiB at %#lx\n", (unsigned long)(KHEAP_SIZE / 1024),
            (uint64_t)KHEAP_BASE);
}

/* kmalloc/kfree run with interrupts off for the same reason as the PMM:
 * dead tasks are freed from the timer IRQ. */
void *kmalloc(size_t size) {
    uint64_t flags = irq_save();
    void *p = heap_arena_alloc(&arena, size);
    irq_restore(flags);
    if (!p && size) kprintf("ATOS: kmalloc: out of heap space (requested %lu bytes)\n", size);
    return p;
}

void kfree(void *ptr) {
    uint64_t flags = irq_save();
    heap_arena_free(&arena, ptr);
    irq_restore(flags);
}

size_t heap_free_bytes(void) {
    uint64_t flags = irq_save();
    size_t total = heap_arena_free_bytes(&arena);
    irq_restore(flags);
    return total;
}

#include "heap.h"
#include "pmm.h"
#include "vmm.h"
#include "../arch/x86_64/cpu.h"
#include "../lib/kprintf.h"

/* Far above the HHDM range (which only spans actual installed RAM) and the
 * kernel image, so it can't collide with either. */
#define KHEAP_BASE 0xFFFFA00000000000ULL
/* Fixed-size to start -- simple and "enough to start" per the plan; making
 * this demand-grow into a larger reserved virtual range is a documented
 * follow-up once something actually pressures it. */
#define KHEAP_SIZE (1 * 1024 * 1024ULL)
#define ALIGNMENT  16ULL

struct block_header {
    size_t size; /* usable size, excluding this header */
    int free;
    struct block_header *prev; /* address-order neighbors, for coalescing */
    struct block_header *next;
};

static struct block_header *heap_head;

static size_t align_up(size_t n, size_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

void heap_init(void) {
    uint64_t pages = KHEAP_SIZE / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        vmm_map(KHEAP_BASE + i * PAGE_SIZE, phys, VMM_PRESENT | VMM_WRITABLE | VMM_NX);
    }

    heap_head = (struct block_header *)KHEAP_BASE;
    heap_head->size = KHEAP_SIZE - sizeof(struct block_header);
    heap_head->free = 1;
    heap_head->prev = NULL;
    heap_head->next = NULL;

    kprintf("ATOS: kernel heap: %lu KiB at %#lx\n", (unsigned long)(KHEAP_SIZE / 1024),
            (uint64_t)KHEAP_BASE);
}

static void split_block(struct block_header *b, size_t size) {
    size_t remaining = b->size - size;
    if (remaining <= sizeof(struct block_header) + ALIGNMENT) return; /* not worth it */

    struct block_header *new_block = (struct block_header *)((uint8_t *)(b + 1) + size);
    new_block->size = remaining - sizeof(struct block_header);
    new_block->free = 1;
    new_block->prev = b;
    new_block->next = b->next;
    if (b->next) b->next->prev = new_block;
    b->next = new_block;
    b->size = size;
}

/* kmalloc/kfree run with interrupts off for the same reason as the PMM:
 * dead tasks are freed from the timer IRQ. */
void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = align_up(size, ALIGNMENT);

    uint64_t flags = irq_save();
    for (struct block_header *b = heap_head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            irq_restore(flags);
            return (void *)(b + 1);
        }
    }
    irq_restore(flags);

    kprintf("ATOS: kmalloc: out of heap space (requested %lu bytes)\n", size);
    return NULL;
}

static void try_merge(struct block_header *a, struct block_header *b) {
    if (!a || !b || !a->free || !b->free) return;
    a->size += sizeof(struct block_header) + b->size;
    a->next = b->next;
    if (b->next) b->next->prev = a;
}

void kfree(void *ptr) {
    if (!ptr) return;
    uint64_t flags = irq_save();
    struct block_header *b = (struct block_header *)ptr - 1;
    b->free = 1;
    try_merge(b, b->next);
    try_merge(b->prev, b);
    irq_restore(flags);
}

size_t heap_free_bytes(void) {
    uint64_t flags = irq_save();
    size_t total = 0;
    for (struct block_header *b = heap_head; b; b = b->next) {
        if (b->free) total += b->size;
    }
    irq_restore(flags);
    return total;
}

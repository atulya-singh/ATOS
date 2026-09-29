#include "heap.h"
#include "heap_core.h"
#include "pmm.h"
#include "vmm.h"
#include "../lib/spinlock.h"
#include "../lib/kprintf.h"
#include "../lib/panic.h"

/* Far above the HHDM range (which only spans actual installed RAM) and the
 * kernel image, so it can't collide with either. */
#define KHEAP_BASE 0xFFFFA00000000000ULL
/* The heap starts small and grows on demand, a page-aligned chunk at a
 * time, up to KHEAP_MAX. The page tables for the whole window are
 * allocated at init (64 page-table pages for 128 MiB), so growing only
 * writes leaf entries: no allocation inside the PMM's accounting other
 * than the heap pages themselves, and no race on the kernel's tables.
 * The heap never shrinks: freed memory stays in the arena for reuse. */
#define KHEAP_INITIAL  (1024 * 1024ULL)
#define KHEAP_MAX      (128 * 1024 * 1024ULL)
#define KHEAP_GROW_MIN (256 * 1024ULL)

static struct heap_arena arena;
static struct spinlock heap_lock;
static uint64_t heap_size;  /* bytes mapped at KHEAP_BASE */
static uint64_t heap_grown; /* bytes added since heap_init */

/* Maps `bytes` more at the heap's end. 0 on success; on failure, maps
 * nothing. */
static int map_more(uint64_t bytes) {
    for (uint64_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            while (off) {
                off -= PAGE_SIZE;
                uint64_t va = KHEAP_BASE + heap_size + off, pa;
                if (vmm_translate(va, &pa)) pmm_free_page(pa & ~(PAGE_SIZE - 1));
                vmm_unmap(va);
            }
            return -1;
        }
        vmm_map(KHEAP_BASE + heap_size + off, phys, VMM_PRESENT | VMM_WRITABLE | VMM_NX);
    }
    return 0;
}

void heap_init(void) {
    vmm_prealloc_tables(KHEAP_BASE, KHEAP_MAX);
    if (map_more(KHEAP_INITIAL)) panic("kernel heap: no memory for the initial %lu KiB", (unsigned long)(KHEAP_INITIAL / 1024));
    heap_size = KHEAP_INITIAL;
    heap_arena_init(&arena, (void *)KHEAP_BASE, KHEAP_INITIAL);
    kprintf("ATOS: kernel heap: %lu KiB at %#lx, growable to %lu MiB\n",
            (unsigned long)(KHEAP_INITIAL / 1024), (uint64_t)KHEAP_BASE,
            (unsigned long)(KHEAP_MAX / (1024 * 1024)));
}

/* Grows the heap enough to satisfy an allocation of `size`. Heap lock
 * held (interrupts off): mapping only touches leaf entries and the PMM. */
static int grow(size_t size) {
    /* Room for the block even if the arena's last block is in use. */
    uint64_t need = (size + 2 * HEAP_HEADER_SIZE + HEAP_ALIGNMENT + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t bytes = need < KHEAP_GROW_MIN ? KHEAP_GROW_MIN : need;
    if (bytes > KHEAP_MAX - heap_size) bytes = KHEAP_MAX - heap_size;
    if (bytes < need) return -1;
    if (map_more(bytes)) {
        /* The generous step didn't fit in free RAM; try the bare minimum. */
        if (bytes == need || map_more(need)) return -1;
        bytes = need;
    }
    heap_arena_grow(&arena, bytes);
    heap_size += bytes;
    heap_grown += bytes;
    return 0;
}

/* Shared by every CPU; interrupts off while held, as with the PMM. */
void *kmalloc(size_t size) {
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    void *p = heap_arena_alloc(&arena, size);
    if (!p && size && size < KHEAP_MAX && grow(size) == 0) p = heap_arena_alloc(&arena, size);
    spin_unlock_irqrestore(&heap_lock, flags);
    if (!p && size) kprintf("ATOS: kmalloc: out of heap space (requested %lu bytes)\n", size);
    return p;
}

void kfree(void *ptr) {
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    heap_arena_free(&arena, ptr);
    spin_unlock_irqrestore(&heap_lock, flags);
}

size_t heap_free_bytes(void) {
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    size_t total = heap_arena_free_bytes(&arena);
    spin_unlock_irqrestore(&heap_lock, flags);
    return total;
}

uint64_t heap_grown_bytes(void) {
    return __atomic_load_n(&heap_grown, __ATOMIC_RELAXED);
}

uint64_t heap_size_bytes(void) {
    return __atomic_load_n(&heap_size, __ATOMIC_RELAXED);
}

int heap_check(void) {
    uint64_t flags = spin_lock_irqsave(&heap_lock);
    int ok = heap_arena_check(&arena);
    spin_unlock_irqrestore(&heap_lock, flags);
    return ok;
}

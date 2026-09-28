#include "pmm.h"
#include "boot_info.h"
#include "../arch/x86_64/cpu.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"

#define BITMAP_BYTE(i) ((i) / 8)
#define BITMAP_BIT(i)  (1u << ((i) % 8))

static uint8_t *bitmap;      /* HHDM-mapped */
static uint64_t bitmap_bits; /* one bit per physical page below highest_addr */

static uint64_t total_pages;
static uint64_t free_pages;

static int bitmap_test(uint64_t idx) {
    return bitmap[BITMAP_BYTE(idx)] & BITMAP_BIT(idx);
}

static void bitmap_set(uint64_t idx) {
    bitmap[BITMAP_BYTE(idx)] |= BITMAP_BIT(idx);
}

static void bitmap_clear(uint64_t idx) {
    bitmap[BITMAP_BYTE(idx)] &= (uint8_t)~BITMAP_BIT(idx);
}

void pmm_init(void) {
    struct limine_memmap_response *memmap = boot_info_memmap();

    uint64_t highest_addr = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        uint64_t end = e->base + e->length;
        if (end > highest_addr) highest_addr = end;
    }

    bitmap_bits = (highest_addr + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t bitmap_size = (bitmap_bits + 7) / 8;

    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_size) {
            bitmap_phys = e->base;
            break;
        }
    }
    if (bitmap_phys == 0) {
        kprintf("ATOS: fatal: no usable region large enough for the PMM bitmap\n");
        for (;;) asm volatile("cli; hlt");
    }

    bitmap = phys_to_virt(bitmap_phys);
    memset(bitmap, 0xFF, bitmap_size); /* default: everything used */

    total_pages = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t start_page = e->base / PAGE_SIZE;
        uint64_t page_count = e->length / PAGE_SIZE;
        for (uint64_t p = 0; p < page_count; p++) bitmap_clear(start_page + p);
        total_pages += page_count;
    }

    /* Reserve the pages the bitmap itself occupies. */
    uint64_t bitmap_pages = (bitmap_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint64_t bitmap_start_page = bitmap_phys / PAGE_SIZE;
    for (uint64_t p = 0; p < bitmap_pages; p++) {
        if (!bitmap_test(bitmap_start_page + p)) {
            bitmap_set(bitmap_start_page + p);
            total_pages--;
        }
    }

    /* Never hand out physical page 0, so callers can use 0 as "no page". */
    if (!bitmap_test(0)) {
        bitmap_set(0);
        total_pages--;
    }

    free_pages = total_pages;

    kprintf("ATOS: PMM: %lu MiB usable (%lu pages)\n",
            (unsigned long)((total_pages * PAGE_SIZE) / (1024 * 1024)),
            (unsigned long)total_pages);
}

/* Both entry points run with interrupts off: the scheduler frees a dead
 * task's pages from the timer IRQ, which may land mid-allocation. */
uint64_t pmm_alloc_page(void) {
    uint64_t flags = irq_save();
    uint64_t phys = 0;
    for (uint64_t i = 0; i < bitmap_bits; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            free_pages--;
            phys = i * PAGE_SIZE;
            break;
        }
    }
    irq_restore(flags);

    if (phys) memset(phys_to_virt(phys), 0, PAGE_SIZE); /* page is ours now */
    return phys;
}

uint64_t pmm_alloc_contiguous(uint64_t count) {
    if (count == 0) return 0;
    uint64_t flags = irq_save();
    uint64_t run_start = 0, run_len = 0, phys = 0;
    for (uint64_t i = 1; i < bitmap_bits; i++) { /* page 0 is never handed out */
        if (bitmap_test(i)) {
            run_len = 0;
            continue;
        }
        if (run_len++ == 0) run_start = i;
        if (run_len == count) {
            for (uint64_t p = run_start; p < run_start + count; p++) bitmap_set(p);
            free_pages -= count;
            phys = run_start * PAGE_SIZE;
            break;
        }
    }
    irq_restore(flags);

    if (phys) memset(phys_to_virt(phys), 0, count * PAGE_SIZE);
    return phys;
}

void pmm_free_page(uint64_t phys) {
    uint64_t flags = irq_save();
    uint64_t idx = phys / PAGE_SIZE;
    if (idx < bitmap_bits && bitmap_test(idx)) { /* else bad addr / double free */
        bitmap_clear(idx);
        free_pages++;
    }
    irq_restore(flags);
}

uint64_t pmm_total_pages(void) { return total_pages; }
uint64_t pmm_free_page_count(void) { return free_pages; }

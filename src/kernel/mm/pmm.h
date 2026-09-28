#pragma once
#include <stdint.h>

#define PAGE_SIZE 0x1000ULL

void pmm_init(void);

/* Returns the physical address of a freshly zeroed page, or 0 on
 * exhaustion (0 is never itself a valid allocation -- see pmm.c). */
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys);
/* `count` physically contiguous zeroed pages (for device DMA structures
 * that must not straddle discontiguous frames), or 0 if no run is free.
 * Release with pmm_free_page on each page. */
uint64_t pmm_alloc_contiguous(uint64_t count);

uint64_t pmm_total_pages(void);
uint64_t pmm_free_page_count(void);

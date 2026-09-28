#pragma once
#include <stdint.h>

#define PAGE_SIZE 0x1000ULL

void pmm_init(void);

/* Returns the physical address of a freshly zeroed page, or 0 on
 * exhaustion (0 is never itself a valid allocation -- see pmm.c). */
uint64_t pmm_alloc_page(void);
void pmm_free_page(uint64_t phys);

uint64_t pmm_total_pages(void);
uint64_t pmm_free_page_count(void);

#pragma once
#include <stddef.h>
#include <stdint.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
/* Sum of free block payloads -- for leak checks, not an allocation hint. */
size_t heap_free_bytes(void);
/* Bytes the heap has grown by since boot. Every one of them came from the
 * PMM (a page each per PAGE_SIZE), so leak checks that span a growth
 * subtract this out. */
uint64_t heap_grown_bytes(void);
/* Bytes currently mapped for the heap. */
uint64_t heap_size_bytes(void);
/* heap_arena_check on the kernel heap: 1 if its structure is sound. */
int heap_check(void);

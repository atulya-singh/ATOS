#pragma once
#include <stddef.h>

/* First-fit allocator over one contiguous region, with address-ordered
 * blocks that coalesce on free. Knows nothing about paging or locking
 * (heap.c adds both), so tests/host can exercise it natively. */

struct heap_block;

struct heap_arena {
    struct heap_block *head;
};

#define HEAP_ALIGNMENT 16

void heap_arena_init(struct heap_arena *h, void *base, size_t size);
void *heap_arena_alloc(struct heap_arena *h, size_t size);
void heap_arena_free(struct heap_arena *h, void *ptr);
/* Sum of free block payloads. */
size_t heap_arena_free_bytes(const struct heap_arena *h);
/* Structural self-check: links agree, blocks are contiguous, and no two
 * free blocks are adjacent (they should have merged). 1 if consistent. */
int heap_arena_check(const struct heap_arena *h);

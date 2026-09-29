#include "heap_core.h"
#include <stdint.h>

struct heap_block {
    size_t size; /* usable size, excluding this header */
    int free;
    struct heap_block *prev; /* address-order neighbors, for coalescing */
    struct heap_block *next;
};

_Static_assert(sizeof(struct heap_block) % HEAP_ALIGNMENT == 0,
               "headers must keep payloads aligned");

static size_t align_up(size_t n, size_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

void heap_arena_init(struct heap_arena *h, void *base, size_t size) {
    h->head = base;
    h->head->size = size - sizeof(struct heap_block);
    h->head->free = 1;
    h->head->prev = NULL;
    h->head->next = NULL;
}

static void split_block(struct heap_block *b, size_t size) {
    size_t remaining = b->size - size;
    if (remaining <= sizeof(struct heap_block) + HEAP_ALIGNMENT) return; /* not worth it */

    struct heap_block *new_block = (struct heap_block *)((uint8_t *)(b + 1) + size);
    new_block->size = remaining - sizeof(struct heap_block);
    new_block->free = 1;
    new_block->prev = b;
    new_block->next = b->next;
    if (b->next) b->next->prev = new_block;
    b->next = new_block;
    b->size = size;
}

void *heap_arena_alloc(struct heap_arena *h, size_t size) {
    if (size == 0 || size > SIZE_MAX - HEAP_ALIGNMENT) return NULL;
    size = align_up(size, HEAP_ALIGNMENT);
    for (struct heap_block *b = h->head; b; b = b->next) {
        if (b->free && b->size >= size) {
            split_block(b, size);
            b->free = 0;
            return b + 1;
        }
    }
    return NULL;
}

static void try_merge(struct heap_block *a, struct heap_block *b) {
    if (!a || !b || !a->free || !b->free) return;
    a->size += sizeof(struct heap_block) + b->size;
    a->next = b->next;
    if (b->next) b->next->prev = a;
}

void heap_arena_free(struct heap_arena *h, void *ptr) {
    (void)h;
    if (!ptr) return;
    struct heap_block *b = (struct heap_block *)ptr - 1;
    b->free = 1;
    try_merge(b, b->next);
    try_merge(b->prev, b);
}

size_t heap_arena_free_bytes(const struct heap_arena *h) {
    size_t total = 0;
    for (const struct heap_block *b = h->head; b; b = b->next) {
        if (b->free) total += b->size;
    }
    return total;
}

int heap_arena_check(const struct heap_arena *h) {
    const struct heap_block *prev = NULL;
    for (const struct heap_block *b = h->head; b; prev = b, b = b->next) {
        if (b->prev != prev) return 0;
        if (b->next && (const uint8_t *)b->next != (const uint8_t *)(b + 1) + b->size) return 0;
        if (b->next && b->free && b->next->free) return 0;
    }
    return 1;
}

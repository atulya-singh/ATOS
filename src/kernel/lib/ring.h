#pragma once
#include <stdint.h>

/* Lock-free single-producer/single-consumer byte ring. The producer only
 * writes `head`, the consumer only writes `tail`; acquire/release ordering
 * on those indices is what publishes the bytes in between. One slot stays
 * empty to tell full from empty. Several consumers (or producers) must
 * serialize among themselves. Zero-initialized is empty. */

#define RING_SIZE 256

struct spsc_ring {
    uint32_t head; /* next slot to write */
    uint32_t tail; /* next slot to read */
    uint8_t data[RING_SIZE];
};

/* 0 if the ring is full (the byte is dropped). */
static inline int ring_push(struct spsc_ring *r, uint8_t byte) {
    uint32_t head = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t next = (head + 1) % RING_SIZE;
    if (next == __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE)) return 0;
    r->data[head] = byte;
    __atomic_store_n(&r->head, next, __ATOMIC_RELEASE);
    return 1;
}

/* -1 if the ring is empty. */
static inline int ring_pop(struct spsc_ring *r) {
    uint32_t tail = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    if (tail == __atomic_load_n(&r->head, __ATOMIC_ACQUIRE)) return -1;
    uint8_t byte = r->data[tail];
    __atomic_store_n(&r->tail, (tail + 1) % RING_SIZE, __ATOMIC_RELEASE);
    return byte;
}

static inline int ring_empty(struct spsc_ring *r) {
    return __atomic_load_n(&r->tail, __ATOMIC_RELAXED) == __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
}

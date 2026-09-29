#include "test.h"
#include "lib/ring.h"
#include <pthread.h>

TEST(ring_fifo_and_full) {
    static struct spsc_ring r;
    CHECK(ring_empty(&r));
    CHECK_EQ_INT(ring_pop(&r), -1);
    for (int i = 0; i < RING_SIZE - 1; i++) CHECK_EQ_INT(ring_push(&r, (uint8_t)i), 1);
    CHECK_EQ_INT(ring_push(&r, 0xAA), 0); /* one slot always stays free */
    for (int i = 0; i < RING_SIZE - 1; i++) CHECK_EQ_INT(ring_pop(&r), (uint8_t)i);
    CHECK(ring_empty(&r));
    /* Wraparound. */
    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 200; i++) ring_push(&r, (uint8_t)(i + round));
        for (int i = 0; i < 200; i++) CHECK_EQ_INT(ring_pop(&r), (uint8_t)(i + round));
    }
}

/* One producer thread and one consumer thread hammer the ring; every byte
 * must arrive exactly once, in order. */
#define STRESS_BYTES 2000000
static struct spsc_ring stress_ring;

static void *producer(void *arg) {
    (void)arg;
    for (uint32_t i = 0; i < STRESS_BYTES;) {
        if (ring_push(&stress_ring, (uint8_t)(i * 31))) i++;
    }
    return NULL;
}

TEST(ring_spsc_threads) {
    pthread_t t;
    CHECK_EQ_INT(pthread_create(&t, NULL, producer, NULL), 0);
    uint32_t got = 0, bad = 0;
    while (got < STRESS_BYTES) {
        int b = ring_pop(&stress_ring);
        if (b < 0) continue;
        if (b != (uint8_t)(got * 31)) bad++;
        got++;
    }
    pthread_join(t, NULL);
    CHECK_EQ_INT(bad, 0);
    CHECK(ring_empty(&stress_ring));
}

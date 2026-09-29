#include "test.h"
#include "mm/heap_core.h"
#include <stdint.h>
#include <stdlib.h>

#define ARENA_SIZE (256 * 1024)

static void *new_arena(struct heap_arena *h) {
    void *mem = aligned_alloc(HEAP_ALIGNMENT, ARENA_SIZE);
    heap_arena_init(h, mem, ARENA_SIZE);
    return mem;
}

TEST(heap_basic_alloc_free) {
    struct heap_arena h;
    void *mem = new_arena(&h);
    size_t initial = heap_arena_free_bytes(&h);

    CHECK(heap_arena_alloc(&h, 0) == NULL);
    void *a = heap_arena_alloc(&h, 1);
    void *b = heap_arena_alloc(&h, 100);
    CHECK(a && b && a != b);
    CHECK((uintptr_t)a % HEAP_ALIGNMENT == 0);
    CHECK((uintptr_t)b % HEAP_ALIGNMENT == 0);
    CHECK(heap_arena_check(&h));
    heap_arena_free(&h, a);
    heap_arena_free(&h, b);
    heap_arena_free(&h, NULL);
    CHECK(heap_arena_check(&h));
    CHECK_EQ_INT(heap_arena_free_bytes(&h), initial);
    free(mem);
}

TEST(heap_exhaustion_and_reuse) {
    struct heap_arena h;
    void *mem = new_arena(&h);
    CHECK(heap_arena_alloc(&h, ARENA_SIZE) == NULL); /* header doesn't fit */
    CHECK(heap_arena_alloc(&h, SIZE_MAX) == NULL);   /* no overflow in rounding */
    void *all = heap_arena_alloc(&h, heap_arena_free_bytes(&h));
    CHECK(all != NULL);
    CHECK(heap_arena_alloc(&h, 16) == NULL);
    heap_arena_free(&h, all);
    CHECK(heap_arena_alloc(&h, 16) != NULL);
    free(mem);
}

TEST(heap_coalesces_both_neighbors) {
    struct heap_arena h;
    void *mem = new_arena(&h);
    size_t initial = heap_arena_free_bytes(&h);
    void *a = heap_arena_alloc(&h, 64);
    void *b = heap_arena_alloc(&h, 64);
    void *c = heap_arena_alloc(&h, 64);
    heap_arena_free(&h, a);
    heap_arena_free(&h, c);
    CHECK(heap_arena_check(&h));
    heap_arena_free(&h, b); /* merges with a before and c (+ tail) after */
    CHECK(heap_arena_check(&h));
    CHECK_EQ_INT(heap_arena_free_bytes(&h), initial);
    /* After a full merge the whole arena is one block again. */
    CHECK(heap_arena_alloc(&h, initial) != NULL);
    free(mem);
}

/* Random alloc/free churn: every live block keeps its own fill pattern
 * (catching overlap), and the structure stays consistent throughout. */
TEST(heap_random_churn) {
    struct heap_arena h;
    void *mem = new_arena(&h);
    size_t initial = heap_arena_free_bytes(&h);
    enum { SLOTS = 64 };
    uint8_t *ptr[SLOTS] = {0};
    size_t len[SLOTS] = {0};
    srand(12345);
    for (int step = 0; step < 20000; step++) {
        int i = rand() % SLOTS;
        if (ptr[i]) {
            for (size_t k = 0; k < len[i]; k++) {
                if (ptr[i][k] != (uint8_t)i) {
                    CHECK(!"block contents were overwritten");
                    free(mem);
                    return;
                }
            }
            heap_arena_free(&h, ptr[i]);
            ptr[i] = NULL;
        } else {
            len[i] = (size_t)(rand() % 3000) + 1;
            ptr[i] = heap_arena_alloc(&h, len[i]);
            if (ptr[i]) memset(ptr[i], i, len[i]);
        }
        if (step % 97 == 0 && !heap_arena_check(&h)) {
            CHECK(!"heap structure inconsistent");
            break;
        }
    }
    for (int i = 0; i < SLOTS; i++) heap_arena_free(&h, ptr[i]);
    CHECK(heap_arena_check(&h));
    CHECK_EQ_INT(heap_arena_free_bytes(&h), initial);
    free(mem);
}

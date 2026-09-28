#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* First-fit allocator over memory obtained with sbrk. Blocks sit in one
 * address-ordered list and free coalesces with both neighbours. The heap
 * only grows; memory goes back to the kernel when the process exits. */

struct block {
    size_t size; /* payload bytes */
    int free;
    struct block *prev, *next;
};

#define ALIGN 16
#define HEADER ((sizeof(struct block) + ALIGN - 1) & ~(size_t)(ALIGN - 1))
#define MIN_GROW (64 * 1024)

static struct block *head, *tail;

static size_t align_up(size_t n) {
    return (n + ALIGN - 1) & ~(size_t)(ALIGN - 1);
}

static void *payload(struct block *b) { return (char *)b + HEADER; }
static struct block *header_of(void *p) { return (struct block *)((char *)p - HEADER); }

static void split(struct block *b, size_t size) {
    if (b->size < size + HEADER + ALIGN) return;
    struct block *rest = (struct block *)((char *)payload(b) + size);
    rest->size = b->size - size - HEADER;
    rest->free = 1;
    rest->prev = b;
    rest->next = b->next;
    if (b->next) b->next->prev = rest;
    else tail = rest;
    b->next = rest;
    b->size = size;
}

static struct block *grow(size_t size) {
    size_t want = HEADER + size;
    if (want < MIN_GROW) want = MIN_GROW;

    if (tail && tail->free) {
        /* Extend the free block at the end instead of starting a new one. */
        if (sbrk((intptr_t)(size - tail->size)) == (void *)-1) return NULL;
        tail->size = size;
        return tail;
    }
    struct block *b = sbrk((intptr_t)want);
    if (b == (void *)-1) return NULL;
    b->size = want - HEADER;
    b->free = 1;
    b->prev = tail;
    b->next = NULL;
    if (tail) tail->next = b;
    else head = b;
    tail = b;
    return b;
}

void *malloc(size_t size) {
    if (size == 0) return NULL;
    size = align_up(size);
    struct block *b = head;
    while (b && !(b->free && b->size >= size)) b = b->next;
    if (!b && !(b = grow(size))) return NULL;
    split(b, size);
    b->free = 0;
    return payload(b);
}

static void merge_next(struct block *b) {
    struct block *n = b->next;
    if (!n || !n->free) return;
    b->size += HEADER + n->size;
    b->next = n->next;
    if (n->next) n->next->prev = b;
    else tail = b;
}

void free(void *ptr) {
    if (!ptr) return;
    struct block *b = header_of(ptr);
    b->free = 1;
    merge_next(b);
    if (b->prev && b->prev->free) {
        b = b->prev;
        merge_next(b);
    }
}

void *calloc(size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return NULL;
    void *p = malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    struct block *b = header_of(ptr);
    if (b->size >= size) return ptr;
    void *n = malloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, b->size);
    free(ptr);
    return n;
}

int atoi(const char *s) {
    int sign = 1, v = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-' || *s == '+') sign = *s++ == '-' ? -1 : 1;
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return sign * v;
}

void exit(int code) {
    /* Nothing is buffered in userspace yet (printf writes through), so
     * there is nothing to flush before leaving. */
    _exit(code);
}

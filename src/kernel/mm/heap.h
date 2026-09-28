#pragma once
#include <stddef.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
/* Sum of free block payloads -- for leak checks, not an allocation hint. */
size_t heap_free_bytes(void);

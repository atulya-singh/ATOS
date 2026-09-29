#pragma once
#include <stdint.h>

/* Name of the kernel function containing `addr`, and how far into it
 * `addr` is; NULL if `addr` isn't in kernel code. */
const char *ksym_lookup(uint64_t addr, uint64_t *offset);

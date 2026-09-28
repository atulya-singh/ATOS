#pragma once
#include <stddef.h>
#include <stdint.h>

/* Checked access to the current task's user memory. Every pointer a
 * syscall receives goes through one of these before the kernel touches
 * it, so a bad pointer becomes -EFAULT instead of a kernel page fault (or
 * worse, a read of kernel memory on the caller's behalf). */

int user_range_ok(uint64_t addr, uint64_t len, int need_write);
int copy_from_user(void *dst, uint64_t src, size_t len);
int copy_to_user(uint64_t dst, const void *src, size_t len);
/* Copies a NUL-terminated string of at most max-1 chars. Returns its
 * length, -EFAULT, or -ENAMETOOLONG. */
int64_t strncpy_from_user(char *dst, uint64_t src, size_t max);

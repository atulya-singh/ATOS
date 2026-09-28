#include "uaccess.h"
#include "../lib/string.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"
#include <atos/abi.h>

int user_range_ok(uint64_t addr, uint64_t len, int need_write) {
    return vmm_user_range_ok(sched_current()->cr3, addr, len, need_write);
}

int copy_from_user(void *dst, uint64_t src, size_t len) {
    if (!user_range_ok(src, len, 0)) return -EFAULT;
    memcpy(dst, (const void *)src, len);
    return 0;
}

int copy_to_user(uint64_t dst, const void *src, size_t len) {
    if (!user_range_ok(dst, len, 1)) return -EFAULT;
    memcpy((void *)dst, src, len);
    return 0;
}

int64_t strncpy_from_user(char *dst, uint64_t src, size_t max) {
    size_t n = 0;
    while (n < max) {
        /* Validate a page at a time: the string's length isn't known up
         * front, and it may end right before an unmapped page. */
        uint64_t addr = src + n;
        uint64_t page_left = PAGE_SIZE - (addr & (PAGE_SIZE - 1));
        if (!user_range_ok(addr, 1, 0)) return -EFAULT;
        const char *p = (const char *)addr;
        for (uint64_t i = 0; i < page_left && n < max; i++, n++) {
            dst[n] = p[i];
            if (p[i] == '\0') return (int64_t)n;
        }
    }
    return -ENAMETOOLONG;
}

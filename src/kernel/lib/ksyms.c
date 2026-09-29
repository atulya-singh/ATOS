#include "ksyms.h"
#include <stddef.h>

/* Generated at link time by tools/gensyms.sh, sorted by address. */
struct ksym {
    uint64_t addr;
    const char *name;
};
extern const uint64_t ksyms_count;
extern const struct ksym ksyms_table[];
extern const char kernel_text_start[], kernel_text_end[];

const char *ksym_lookup(uint64_t addr, uint64_t *offset) {
    if (addr < (uint64_t)kernel_text_start || addr >= (uint64_t)kernel_text_end) return NULL;
    /* Last symbol at or below addr. */
    uint64_t lo = 0, hi = ksyms_count;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (ksyms_table[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return NULL;
    *offset = addr - ksyms_table[lo - 1].addr;
    return ksyms_table[lo - 1].name;
}

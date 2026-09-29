#pragma once
#include <stdint.h>

#define MSR_APIC_BASE      0x1B
#define MSR_EFER           0xC0000080
#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102 /* swapped with GS_BASE by swapgs */

#define EFER_NXE (1u << 11)

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    asm volatile("wrmsr" : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)), "c"(msr));
}

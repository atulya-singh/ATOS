#include "percpu.h"
#include "msr.h"

struct cpu cpus[MAX_CPUS];
unsigned cpu_count = 1;

void percpu_set(struct cpu *c) {
    c->self = c;
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
}

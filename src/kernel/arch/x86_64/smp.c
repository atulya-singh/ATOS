#include "smp.h"
#include "gdt.h"
#include "idt.h"
#include "lapic.h"
#include "msr.h"
#include "percpu.h"
#include "../../lib/kprintf.h"
#include "../../lib/spinlock.h"
#include "../../mm/heap.h"
#include "../../mm/pmm.h"
#include "../../mm/vmm.h"
#include "../../sched/sched.h"
#include <limine.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_smp_request smp_request = {
    .id = LIMINE_SMP_REQUEST,
    .revision = 0,
    .flags = 0, /* xAPIC, not x2APIC */
};

#define AP_DOUBLE_FAULT_STACK 8192

extern __attribute__((noreturn)) void smp_enter_kernel(uint64_t cr3, uint64_t rsp,
                                                       void (*fn)(struct cpu *), struct cpu *c);

/* Runs on the AP's own idle stack, under the kernel's page tables. */
static __attribute__((noreturn)) void ap_main(struct cpu *c) {
    percpu_set(c); /* spinlocks (so kmalloc too) need this_cpu() */
    uint8_t *df_stack = kmalloc(AP_DOUBLE_FAULT_STACK);
    gdt_init_cpu(c, (uint64_t)(df_stack + AP_DOUBLE_FAULT_STACK));
    percpu_set(c); /* the GDT load reset %gs */
    idt_load();
    lapic_enable_cpu();
    c->current = c->idle;
    lapic_timer_start();
    kprintf("ATOS: smp: CPU %u (LAPIC id %u) online\n", c->index, c->apic_id);
    __atomic_store_n(&c->online, 1, __ATOMIC_RELEASE);

    /* This is now the CPU's idle task: from the first timer tick on,
     * schedule() hands the CPU to whatever is runnable. */
    asm volatile("sti");
    for (;;) asm volatile("hlt");
}

/* Limine starts each AP here, in long mode on its own small stack and
 * Limine's page tables. Match the BSP's CPU setup, then move onto ours.
 * Only the kernel image is mapped until the CR3 switch -- not the heap --
 * so everything needed comes from `c`, which lives in .bss. */
static void ap_entry(struct limine_smp_info *info) {
    struct cpu *c = (struct cpu *)info->extra_argument;
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_NXE); /* before any NX PTE is walked */
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    asm volatile("mov %0, %%cr0" : : "r"(cr0 | (1ULL << 16))); /* WP: honor read-only pages */
    smp_enter_kernel(vmm_kernel_cr3(), c->boot_stack_top, ap_main, c);
}

void smp_init(void) {
    struct limine_smp_response *resp = smp_request.response;
    cpus[0].apic_id = lapic_id();
    cpus[0].online = 1;
    if (!resp) {
        kprintf("ATOS: smp: no MP response from the bootloader, running on 1 CPU\n");
        return;
    }

    for (uint64_t i = 0; i < resp->cpu_count; i++) {
        struct limine_smp_info *info = resp->cpus[i];
        if (info->lapic_id == resp->bsp_lapic_id) continue;
        if (cpu_count == MAX_CPUS) {
            kprintf("ATOS: smp: more than %d CPUs, ignoring the rest\n", MAX_CPUS);
            break;
        }
        struct cpu *c = &cpus[cpu_count];
        c->self = c;
        c->index = cpu_count;
        c->apic_id = info->lapic_id;
        c->idle = task_create_idle(c->index);
        if (!c->idle) {
            kprintf("ATOS: smp: out of memory for CPU %u\n", c->index);
            break;
        }
        c->boot_stack_top = c->idle->kstack_top;
        cpu_count++;
        info->extra_argument = (uint64_t)c;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_SEQ_CST);
        while (!__atomic_load_n(&c->online, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
    }
    kprintf("ATOS: smp: %u CPU(s) online\n", cpu_count);
}

/* --- TLB shootdown --- */

static struct spinlock shootdown_lock;
static volatile uint64_t shootdown_start, shootdown_len;
static volatile uint32_t shootdown_pending;

void tlb_shootdown(uint64_t start, uint64_t len) {
    if (cpu_count == 1) return;
    spin_lock(&shootdown_lock);
    shootdown_start = start;
    shootdown_len = len;
    /* Targeted IPIs, not a broadcast: a CPU still parked in the
     * bootloader would otherwise take a stale one once it came up. */
    uint32_t me = this_cpu()->index, targets = 0;
    uint32_t target_ids[MAX_CPUS];
    for (unsigned i = 0; i < cpu_count; i++) {
        if (i != me && __atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE)) {
            target_ids[targets++] = cpus[i].apic_id;
        }
    }
    __atomic_store_n(&shootdown_pending, targets, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < targets; i++) lapic_send_ipi(target_ids[i], VEC_TLB_SHOOTDOWN);
    while (__atomic_load_n(&shootdown_pending, __ATOMIC_ACQUIRE)) __builtin_ia32_pause();
    spin_unlock(&shootdown_lock);
}

void tlb_shootdown_ipi(void) {
    uint64_t start = shootdown_start & ~(PAGE_SIZE - 1);
    for (uint64_t va = start; va < shootdown_start + shootdown_len; va += PAGE_SIZE) {
        asm volatile("invlpg (%0)" : : "r"(va) : "memory");
    }
    __atomic_sub_fetch(&shootdown_pending, 1, __ATOMIC_RELEASE);
}

void smp_halt_others(void) {
    if (cpu_count > 1) lapic_broadcast_nmi();
}

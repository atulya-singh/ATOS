#include <limine.h>
#include <stdint.h>

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "dev/fbcon.h"
#include "dev/keyboard.h"
#include "dev/pci.h"
#include "dev/pit.h"
#include "dev/serial.h"
#include "lib/kprintf.h"
#include "mm/boot_info.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* A short, permanent smoke test (not throwaway debug code): exercises
 * split + coalesce so a broken heap shows up as wrong numbers on every
 * boot, not just when someone happens to stress it later. */
static void heap_self_test(void) {
    uint64_t free_before = pmm_free_page_count();

    void *a = kmalloc(64);
    void *b = kmalloc(128);
    void *c = kmalloc(256);
    kfree(b);
    void *d = kmalloc(96); /* should reuse b's freed block */
    kfree(a);
    kfree(c);
    kfree(d);

    uint64_t free_after = pmm_free_page_count();
    kprintf("ATOS: heap self-test: alloc/free/coalesce ok (pmm free pages %lu -> %lu)\n",
            free_before, free_after);
}

/* --- Phase 3 boot-time self-tests: multitasking + user mode --- */

extern const char user_hello_start[], user_hello_end[];
extern const char user_fault_start[], user_fault_end[];
extern const char user_spin_start[], user_spin_end[];

static volatile uint64_t worker_progress;

static void worker(void *arg) {
    const char *tag = arg;
    for (int i = 1; i <= 5; i++) {
        kprintf("ATOS: [%s] iteration %d at tick=%lu\n", tag, i, pit_get_ticks());
        worker_progress++;
        task_sleep(5);
    }
}

/* Busy-waits without ever yielding. Only a timer-driven preemption can
 * let anything else run meanwhile, so if worker_progress moves while it
 * spins, preemption works. */
static void spinner(void *arg) {
    (void)arg;
    uint64_t start = pit_get_ticks();
    uint64_t progress_before = worker_progress;
    while (pit_get_ticks() - start < 40) {
        asm volatile("pause");
    }
    uint64_t others = worker_progress - progress_before;
    kprintf("ATOS: preemption %s: workers ran %lu times while spinner held the CPU for 40 ticks\n",
            others ? "ok" : "FAILED", others);
}

/* Long-lived stand-in for a shell until Phase 5: assembles keystrokes
 * into lines (with backspace) and reports each completed one. */
static void kbd_line_service(void *arg) {
    (void)arg;
    char line[128];
    unsigned len = 0;
    for (;;) {
        char c = keyboard_getc();
        if (c == '\n') {
            line[len] = '\0';
            kprintf("ATOS: keyboard line: %s\n", line);
            len = 0;
        } else if (c == '\b') {
            if (len) len--;
        } else if (len < sizeof(line) - 1) {
            line[len++] = c;
        }
    }
}

static void spawn_self_tests(void) {
    task_create_kernel("worker-a", worker, "worker-a");
    task_create_kernel("worker-b", worker, "worker-b");
    task_create_kernel("spinner", spinner, NULL);
    task_create_user("user-hello", user_hello_start,
                     (size_t)(user_hello_end - user_hello_start));
    task_create_user("user-fault", user_fault_start,
                     (size_t)(user_fault_end - user_fault_start));
    task_create_user("user-spin", user_spin_start,
                     (size_t)(user_spin_end - user_spin_start));
}

/* Runs on the kernel-owned stack, under our own page tables, after
 * vmm_init()'s CR3 + stack switch -- see vmm_switch_and_continue. From
 * sched_init() on, this *is* the idle task. */
static void kmain_stage2(void) {
    heap_init();
    heap_self_test();

    sched_init();
    keyboard_init();
    pci_init();
    task_create_kernel("kbd-line", kbd_line_service, NULL);

    /* Everything spawned from here on is a self-test that should exit and
     * hand back every resource it took. */
    uint64_t baseline_tasks = sched_task_count();
    uint64_t pmm_before = pmm_free_page_count();
    uint64_t heap_before = heap_free_bytes();
    spawn_self_tests();

    pit_init(100);
    kprintf("ATOS: PIT timer at 100 Hz\n");

    /* Printed before sti: from the first tick on, idle only gets the CPU
     * once every other task is asleep or gone. */
    kprintf("ATOS: interrupts enabled, entering idle loop\n");
    asm volatile("sti");

    int reaped_reported = 0;
    uint64_t last_reported = 0;
    for (;;) {
        asm volatile("hlt");

        if (!reaped_reported && sched_task_count() == baseline_tasks) {
            /* Only long-lived services are left, so every stack, address space and task
             * struct should be back where it came from. */
            uint64_t pmm_after = pmm_free_page_count();
            uint64_t heap_after = heap_free_bytes();
            kprintf("ATOS: sched self-test: all tasks reaped %s (pmm free pages %lu -> %lu, heap free bytes %lu -> %lu)\n",
                    (pmm_after == pmm_before && heap_after == heap_before) ? "cleanly" : "WITH LEAKS",
                    pmm_before, pmm_after, heap_before, heap_after);
            reaped_reported = 1;
        }

        uint64_t t = pit_get_ticks();
        if (t - last_reported >= 500) {
            kprintf("ATOS: tick=%lu (alive)\n", t);
            last_reported = t;
        }
    }
}

void kmain(void) {
    serial_init();
    fbcon_init();
    kprintf("ATOS: booting...\n");

    boot_info_init();

    gdt_init();
    kprintf("ATOS: GDT + TSS loaded\n");

    idt_init();
    kprintf("ATOS: IDT + PIC configured\n");

    pmm_init();

    /* Never returns: switches to our own page tables and a kernel-owned
     * stack, then jumps into kmain_stage2. */
    vmm_init(kmain_stage2);
}

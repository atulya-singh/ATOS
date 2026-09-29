#include <limine.h>
#include <stdint.h>

#include "arch/x86_64/gdt.h"
#include "crashtest.h"
#include "arch/x86_64/idt.h"
#include "dev/fbcon.h"
#include "dev/keyboard.h"
#include "dev/pci.h"
#include "dev/pit.h"
#include "dev/serial.h"
#include "dev/virtio_blk.h"
#include "dev/block.h"
#include "fs/devfs.h"
#include "fs/fat.h"
#include "fs/initrd.h"
#include "lib/kprintf.h"
#include "mm/boot_info.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "selftest.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* Mounts the first block device holding FAT32 at /disk. */
static void mount_disk(void) {
    struct block_device *dev;
    for (unsigned i = 0; (dev = block_device_at(i)) != NULL; i++) {
        if (fat_mount(dev, "/disk") == 0) return;
    }
}

/* Runs on the kernel-owned stack, under our own page tables, after
 * vmm_init()'s CR3 + stack switch -- see vmm_switch_and_continue. From
 * sched_init() on, this *is* the idle task. */
static void kmain_stage2(void) {
    heap_init();
    selftest_heap();

    sched_init();
    initrd_init();
    devfs_init();
    keyboard_init();
    pci_init();
    virtio_blk_init();
    mount_disk();
    selftest_spawn();
    crashtest_start(boot_info_cmdline());

    pit_init(100);
    kprintf("ATOS: PIT timer at 100 Hz\n");

    /* Printed before sti: from the first tick on, idle only gets the CPU
     * once every other task is asleep or gone. */
    kprintf("ATOS: interrupts enabled, entering idle loop\n");
    asm volatile("sti");

    /* Userspace starts only once the kernel self-tests are done: their
     * exact leak accounting needs the machine to themselves. */
    int init_started = 0;
    uint64_t last_reported = 0;
    for (;;) {
        asm volatile("hlt");
        if (selftest_poll() && !init_started) {
            static const char *const init_argv[] = {"/bin/init"};
            kprintf("ATOS: self-tests done, starting /bin/init\n");
            process_spawn("/bin/init", 1, init_argv);
            init_started = 1;
        }

        /* Liveness heartbeat, only until userspace owns the console:
         * after that the shell prompt is the proof of life, and kernel
         * chatter would land in the middle of the user's typing. */
        uint64_t t = pit_get_ticks();
        if (!init_started && t - last_reported >= 500) {
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

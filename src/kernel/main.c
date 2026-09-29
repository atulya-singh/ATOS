#include <limine.h>
#include <stdint.h>

#include "acpi/acpi.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/ioapic.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/smp.h"
#include "crashtest.h"
#include "dev/block.h"
#include "dev/fbcon.h"
#include "dev/keyboard.h"
#include "dev/pci.h"
#include "dev/rtc.h"
#include "dev/serial.h"
#include "dev/timer.h"
#include "dev/virtio_blk.h"
#include "dev/virtio_net.h"
#include "fs/devfs.h"
#include "fs/fat.h"
#include "fs/initrd.h"
#include "lib/cmdline.h"
#include "lib/io.h"
#include "lib/kprintf.h"
#include "lib/panic.h"
#include "mm/boot_info.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/net.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "selftest.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

/* QEMU's isa-debug-exit device (-device isa-debug-exit,iobase=0xf4) ends
 * the emulator with status (code << 1) | 1: 1 for pass, 3 for fail.
 * Without the device, the write does nothing and we just halt. */
static void qemu_debug_exit(int code) {
    outl(0xF4, (uint32_t)code);
    for (;;) asm volatile("cli; hlt");
}

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

    acpi_init();
    rtc_set_century_register(acpi_rtc_century_register());

    /* Interrupt routing moves from the legacy PICs to the APICs. */
    const struct acpi_madt_info *madt = acpi_madt();
    if (!madt || madt->ioapic_count == 0) panic("no ACPI MADT with a local APIC and an I/O APIC");
    lapic_init(madt->lapic_addr);
    lapic_enable_cpu();
    cpus[0].apic_id = lapic_id();
    ioapic_init();

    sched_init();
    initrd_init();
    devfs_init();
    keyboard_init();
    pci_init();
    virtio_blk_init();
    virtio_net_init();
    mount_disk();
    net_init();

    lapic_timer_calibrate(TIMER_HZ);
    smp_init();

    selftest_spawn();
    crashtest_start();
    /* "selftest-exit": a headless CI run. Instead of starting userspace,
     * leave QEMU with the self-test verdict as its exit status. */
    int selftest_exit = cmdline_has("selftest-exit");
    /* "init=<path>": run something other than /bin/init as the first process. */
    static char init_path[64] = "/bin/init";
    cmdline_get("init", init_path, sizeof(init_path));

    lapic_timer_start();

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
            if (selftest_exit) qemu_debug_exit(selftest_failures() ? 1 : 0);
            const char *init_argv[] = {init_path};
            kprintf("ATOS: self-tests done, starting %s\n", init_path);
            if (!process_spawn(init_path, 1, init_argv)) {
                kprintf("ATOS: could not start %s\n", init_path);
            }
            init_started = 1;
        }

        /* Liveness heartbeat, only until userspace owns the console:
         * after that the shell prompt is the proof of life, and kernel
         * chatter would land in the middle of the user's typing. */
        uint64_t t = timer_ticks();
        if (!init_started && t - last_reported >= 500) {
            kprintf("ATOS: tick=%lu (alive)\n", t);
            last_reported = t;
        }
    }
}

void kmain(void) {
    percpu_set(&cpus[0]); /* before anything that might take a spinlock */
    serial_init();
    fbcon_init();
    kprintf("ATOS: booting...\n");

    boot_info_init();

    gdt_init();
    kprintf("ATOS: GDT + TSS loaded\n");

    idt_init();
    kprintf("ATOS: IDT + PIC configured\n");

    fpu_init_cpu();

    pmm_init();

    /* Never returns: switches to our own page tables and a kernel-owned
     * stack, then jumps into kmain_stage2. */
    vmm_init(kmain_stage2);
}

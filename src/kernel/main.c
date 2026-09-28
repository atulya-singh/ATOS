#include <limine.h>
#include <stdint.h>

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "dev/pit.h"
#include "dev/serial.h"
#include "lib/kprintf.h"

__attribute__((used, section(".limine_requests")))
static volatile LIMINE_BASE_REVISION(3);

__attribute__((used, section(".limine_requests_start")))
static volatile LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile LIMINE_REQUESTS_END_MARKER;

void kmain(void) {
    serial_init();
    kprintf("ATOS: booting...\n");

    gdt_init();
    kprintf("ATOS: GDT + TSS loaded\n");

    idt_init();
    kprintf("ATOS: IDT + PIC configured\n");

    pit_init(100);
    kprintf("ATOS: PIT timer at 100 Hz\n");

    asm volatile("sti");
    kprintf("ATOS: interrupts enabled, entering idle loop\n");

    uint64_t last_reported = 0;
    for (;;) {
        asm volatile("hlt");
        uint64_t t = pit_get_ticks();
        if (t - last_reported >= 500) {
            kprintf("ATOS: tick=%llu (alive)\n", t);
            last_reported = t;
        }
    }
}

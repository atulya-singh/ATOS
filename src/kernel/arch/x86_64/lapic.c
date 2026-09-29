#include "lapic.h"
#include "idt.h"
#include "msr.h"
#include "../../lib/io.h"
#include "../../lib/kprintf.h"
#include "../../mm/vmm.h"

#define LAPIC_ID          0x020
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SVR         0x0F0
#define LAPIC_ICR_LOW     0x300
#define LAPIC_ICR_HIGH    0x310
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_TIMER_INIT  0x380
#define LAPIC_TIMER_COUNT 0x390
#define LAPIC_TIMER_DIV   0x3E0

#define SVR_ENABLE        (1u << 8)
#define TIMER_PERIODIC    (1u << 17)
#define LVT_MASKED        (1u << 16)
#define TIMER_DIV_16      0x3
#define ICR_PENDING       (1u << 12)
#define ICR_ASSERT        (1u << 14)
#define ICR_NMI           (4u << 8)
#define ICR_ALL_BUT_SELF  (3u << 18)

#define APIC_BASE_ENABLE  (1u << 11)

static volatile uint32_t *regs;
static uint32_t ticks_per_period; /* timer counts per 1/hz s, at divide-by-16 */

static uint32_t rd(uint32_t reg) { return regs[reg / 4]; }
static void wr(uint32_t reg, uint32_t v) { regs[reg / 4] = v; }

void lapic_init(uint64_t phys_addr) {
    regs = vmm_map_phys(phys_addr, 0x1000, VMM_NOCACHE);
}

void lapic_enable_cpu(void) {
    wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | APIC_BASE_ENABLE);
    wr(LAPIC_TPR, 0); /* accept every priority */
    wr(LAPIC_SVR, SVR_ENABLE | VEC_SPURIOUS);
}

uint32_t lapic_id(void) {
    return rd(LAPIC_ID) >> 24;
}

void lapic_eoi(void) {
    wr(LAPIC_EOI, 0);
}

/* PIT channel 2 in one-shot mode is the reference clock: its gate is
 * software-controlled through port 0x61, and bit 5 there reads its output,
 * which goes high on terminal count -- so it can be polled without IRQs. */
#define PIT_FREQUENCY 1193182u

void lapic_timer_calibrate(unsigned hz) {
    uint32_t pit_count = PIT_FREQUENCY / hz;
    outb(0x61, (uint8_t)((inb(0x61) & ~0x02) | 0x01)); /* gate on, speaker off */
    outb(0x43, 0xB0);                                  /* ch 2, lo/hi, mode 0 */
    outb(0x42, (uint8_t)pit_count);
    outb(0x42, (uint8_t)(pit_count >> 8));
    uint8_t gate = inb(0x61) & ~0x01;
    outb(0x61, gate);          /* restart the count: gate low... */
    outb(0x61, gate | 0x01);   /* ...then high */

    wr(LAPIC_TIMER_DIV, TIMER_DIV_16);
    wr(LAPIC_LVT_TIMER, LVT_MASKED);
    wr(LAPIC_TIMER_INIT, 0xFFFFFFFF);
    while (!(inb(0x61) & 0x20)) {}
    uint32_t elapsed = 0xFFFFFFFF - rd(LAPIC_TIMER_COUNT);
    wr(LAPIC_TIMER_INIT, 0);

    ticks_per_period = elapsed;
    kprintf("ATOS: lapic: timer runs at %lu kHz (div 16), %u Hz tick\n",
            (uint64_t)elapsed * hz / 1000, hz);
}

void lapic_timer_start(void) {
    wr(LAPIC_TIMER_DIV, TIMER_DIV_16);
    wr(LAPIC_LVT_TIMER, VEC_TIMER | TIMER_PERIODIC);
    wr(LAPIC_TIMER_INIT, ticks_per_period);
}

static void send_icr(uint32_t high, uint32_t low) {
    while (rd(LAPIC_ICR_LOW) & ICR_PENDING) __builtin_ia32_pause();
    wr(LAPIC_ICR_HIGH, high);
    wr(LAPIC_ICR_LOW, low); /* writing the low half sends it */
    while (rd(LAPIC_ICR_LOW) & ICR_PENDING) __builtin_ia32_pause();
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector) {
    send_icr(apic_id << 24, ICR_ASSERT | vector);
}

void lapic_broadcast_ipi(uint8_t vector) {
    send_icr(0, ICR_ASSERT | ICR_ALL_BUT_SELF | vector);
}

void lapic_broadcast_nmi(void) {
    if (regs) send_icr(0, ICR_ASSERT | ICR_ALL_BUT_SELF | ICR_NMI);
}

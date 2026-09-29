#pragma once
#include <stdint.h>

/* ACPI table discovery: RSDP (from Limine) -> XSDT/RSDT -> the tables the
 * kernel uses: MADT (CPUs, I/O APICs, IRQ overrides) and FADT (power
 * management registers, reset register, RTC century). There is no AML
 * interpreter; the one DSDT object needed, \_S5 (the soft-off sleep type),
 * is found by a byte-pattern scan, as most hobby kernels do. */

#define ACPI_MAX_CPUS    64
#define ACPI_MAX_IOAPICS 4

struct acpi_ioapic {
    uint8_t id;
    uint32_t addr;     /* physical MMIO base */
    uint32_t gsi_base; /* first global system interrupt it handles */
};

/* ISA IRQ -> GSI override (MADT type 2), with MPS INTI polarity/trigger flags. */
struct acpi_irq_override {
    uint8_t irq;
    uint32_t gsi;
    uint16_t flags;
};

#define ACPI_INTI_POLARITY_MASK 0x3
#define ACPI_INTI_ACTIVE_LOW    0x3
#define ACPI_INTI_TRIGGER_MASK  0xC
#define ACPI_INTI_LEVEL         0xC

struct acpi_madt_info {
    uint64_t lapic_addr;
    int has_8259; /* PCAT_COMPAT: legacy PICs present (to be masked) */
    unsigned cpu_count;
    uint8_t cpu_apic_ids[ACPI_MAX_CPUS];
    unsigned ioapic_count;
    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    unsigned override_count;
    struct acpi_irq_override overrides[16];
};

/* Finds and validates the tables; logs what it found. Safe to call when
 * the firmware has no ACPI (everything then reports absent). */
void acpi_init(void);

/* NULL if there was no valid MADT. */
const struct acpi_madt_info *acpi_madt(void);

/* The GSI an ISA IRQ is wired to (identity unless overridden), and the
 * override's INTI flags (0 = bus defaults: edge, active high). */
uint32_t acpi_isa_irq_to_gsi(uint8_t irq, uint16_t *flags);

/* CMOS index of the RTC century register, or 0 if the FADT names none. */
uint8_t acpi_rtc_century_register(void);

/* Enters S5 (soft off). Falls back to QEMU's/Bochs's fixed power-off
 * ports, then halts, if ACPI can't do it. */
__attribute__((noreturn)) void acpi_poweroff(void);

/* Resets the machine: FADT reset register, then the 8042 keyboard
 * controller's reset line, then a deliberate triple fault. */
__attribute__((noreturn)) void acpi_reboot(void);

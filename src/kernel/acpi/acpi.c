#include "acpi.h"
#include "../lib/io.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/boot_info.h"
#include "../mm/vmm.h"
#include <limine.h>
#include <stddef.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST,
    .revision = 0,
};

struct rsdp {
    char signature[8]; /* "RSD PTR " */
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision; /* 0 = ACPI 1.0 (RSDT only), 2+ = has XSDT */
    uint32_t rsdt_addr;
    uint32_t length;
    uint64_t xsdt_addr;
    uint8_t ext_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct gas { /* Generic Address Structure */
    uint8_t space; /* 0 = memory, 1 = I/O port, 2 = PCI config */
    uint8_t bit_width;
    uint8_t bit_offset;
    uint8_t access_size;
    uint64_t address;
} __attribute__((packed));

#define GAS_MEMORY 0
#define GAS_IO     1

struct fadt {
    struct sdt_header h;
    uint32_t firmware_ctrl;
    uint32_t dsdt;
    uint8_t reserved0;
    uint8_t preferred_pm_profile;
    uint16_t sci_int;
    uint32_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t acpi_disable;
    uint8_t s4bios_req;
    uint8_t pstate_cnt;
    uint32_t pm1a_evt_blk, pm1b_evt_blk;
    uint32_t pm1a_cnt_blk, pm1b_cnt_blk;
    uint32_t pm2_cnt_blk, pm_tmr_blk;
    uint32_t gpe0_blk, gpe1_blk;
    uint8_t pm1_evt_len, pm1_cnt_len, pm2_cnt_len, pm_tmr_len;
    uint8_t gpe0_blk_len, gpe1_blk_len, gpe1_base, cst_cnt;
    uint16_t p_lvl2_lat, p_lvl3_lat, flush_size, flush_stride;
    uint8_t duty_offset, duty_width, day_alrm, mon_alrm, century;
    uint16_t iapc_boot_arch;
    uint8_t reserved1;
    uint32_t flags;
    struct gas reset_reg;
    uint8_t reset_value;
    uint16_t arm_boot_arch;
    uint8_t fadt_minor_version;
    uint64_t x_firmware_ctrl;
    uint64_t x_dsdt;
} __attribute__((packed));

#define FADT_RESET_REG_SUP (1u << 10)

#define PM1_SCI_EN     (1u << 0)
#define PM1_SLP_TYP(t) ((uint16_t)(((t) & 7) << 10))
#define PM1_SLP_EN     (1u << 13)

struct madt {
    struct sdt_header h;
    uint32_t lapic_addr;
    uint32_t flags;
    uint8_t entries[];
} __attribute__((packed));

#define MADT_PCAT_COMPAT 1u

enum {
    MADT_LAPIC = 0,
    MADT_IOAPIC = 1,
    MADT_OVERRIDE = 2,
    MADT_LAPIC_ADDR = 5,
};

static int acpi_revision = -1; /* -1: no ACPI */
static const struct fadt *fadt;
static struct acpi_madt_info madt_info;
static int have_madt;
static int s5_found;
static uint8_t s5_typ_a, s5_typ_b;

/* Physical table address -> pointer, mapping it first if it lies outside
 * the memmap (firmware tables usually don't, but nothing guarantees it). */
static const void *map_table(uint64_t phys) {
    const struct sdt_header *h = vmm_map_phys(phys, sizeof(*h), 0);
    return vmm_map_phys(phys, h->length, 0);
}

static int checksum_ok(const void *p, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += ((const uint8_t *)p)[i];
    return sum == 0;
}

/* --- \_S5 from the DSDT ---
 * Its AML is NameOp "_S5_" PackageOp PkgLength NumElements, then the
 * integers SLP_TYPa and SLP_TYPb, each ZeroOp, OneOp, or BytePrefix n. */

static const uint8_t *aml_integer(const uint8_t *p, const uint8_t *end, uint8_t *out) {
    if (p >= end) return NULL;
    switch (*p) {
    case 0x00: *out = 0; return p + 1; /* ZeroOp */
    case 0x01: *out = 1; return p + 1; /* OneOp */
    case 0x0A:                         /* BytePrefix */
        if (p + 1 >= end) return NULL;
        *out = p[1];
        return p + 2;
    default: return NULL;
    }
}

static void find_s5(const struct sdt_header *dsdt) {
    const uint8_t *aml = (const uint8_t *)(dsdt + 1);
    const uint8_t *end = (const uint8_t *)dsdt + dsdt->length;
    for (const uint8_t *p = aml; p + 4 < end; p++) {
        if (memcmp(p, "_S5_", 4) != 0) continue;
        /* NameOp directly before it (or before a root prefix '\'). */
        int named = p > aml && (p[-1] == 0x08 || (p[-1] == '\\' && p - 1 > aml && p[-2] == 0x08));
        if (!named) continue;
        const uint8_t *q = p + 4;
        if (q >= end || *q != 0x12) continue; /* PackageOp */
        q++;
        if (q >= end) return;
        q += 1 + (*q >> 6); /* PkgLength: lead byte + (bits 7:6) more bytes */
        q++;                /* NumElements */
        q = aml_integer(q, end, &s5_typ_a);
        if (q) q = aml_integer(q, end, &s5_typ_b);
        if (q) s5_found = 1;
        return;
    }
}

static void parse_madt(const struct madt *m) {
    madt_info.lapic_addr = m->lapic_addr;
    madt_info.has_8259 = (m->flags & MADT_PCAT_COMPAT) != 0;
    const uint8_t *p = m->entries;
    const uint8_t *end = (const uint8_t *)m + m->h.length;
    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        switch (p[0]) {
        case MADT_LAPIC: {
            uint32_t flags;
            memcpy(&flags, p + 4, 4);
            /* bit 0: enabled (bit 1, online-capable, isn't supported) */
            if ((flags & 1) && madt_info.cpu_count < ACPI_MAX_CPUS) {
                madt_info.cpu_apic_ids[madt_info.cpu_count++] = p[3];
            }
            break;
        }
        case MADT_IOAPIC:
            if (madt_info.ioapic_count < ACPI_MAX_IOAPICS) {
                struct acpi_ioapic *io = &madt_info.ioapics[madt_info.ioapic_count++];
                io->id = p[2];
                memcpy(&io->addr, p + 4, 4);
                memcpy(&io->gsi_base, p + 8, 4);
            }
            break;
        case MADT_OVERRIDE:
            if (madt_info.override_count < 16) {
                struct acpi_irq_override *o = &madt_info.overrides[madt_info.override_count++];
                o->irq = p[3];
                memcpy(&o->gsi, p + 4, 4);
                memcpy(&o->flags, p + 8, 2);
            }
            break;
        case MADT_LAPIC_ADDR:
            memcpy(&madt_info.lapic_addr, p + 4, 8);
            break;
        }
        p += p[1];
    }
    have_madt = 1;
}

void acpi_init(void) {
    struct limine_rsdp_response *resp = rsdp_request.response;
    if (!resp || !resp->address) {
        kprintf("ATOS: acpi: no RSDP from the bootloader, ACPI disabled\n");
        return;
    }
    /* Depending on the protocol revision the address is physical or
     * already in the HHDM; the HHDM starts far above any physical one. */
    uint64_t addr = (uint64_t)resp->address;
    uint64_t rsdp_phys = addr >= (uint64_t)phys_to_virt(0) ? virt_to_phys_hhdm((void *)addr) : addr;
    const struct rsdp *rsdp = vmm_map_phys(rsdp_phys, sizeof(struct rsdp), 0);
    if (memcmp(rsdp->signature, "RSD PTR ", 8) != 0 || !checksum_ok(rsdp, 20)) {
        kprintf("ATOS: acpi: bad RSDP, ACPI disabled\n");
        return;
    }

    int use_xsdt = rsdp->revision >= 2 && rsdp->xsdt_addr && checksum_ok(rsdp, rsdp->length);
    const struct sdt_header *root = map_table(use_xsdt ? rsdp->xsdt_addr : rsdp->rsdt_addr);
    if (!checksum_ok(root, root->length)) {
        kprintf("ATOS: acpi: bad %s checksum, ACPI disabled\n", use_xsdt ? "XSDT" : "RSDT");
        return;
    }
    acpi_revision = rsdp->revision;

    unsigned entry_size = use_xsdt ? 8 : 4;
    unsigned count = (unsigned)((root->length - sizeof(*root)) / entry_size);
    char names[5 * 24 + 1];
    size_t nlen = 0;
    for (unsigned i = 0; i < count; i++) {
        uint64_t phys = 0;
        memcpy(&phys, (const uint8_t *)(root + 1) + i * entry_size, entry_size);
        const struct sdt_header *t = map_table(phys);
        if (!checksum_ok(t, t->length)) continue; /* ignore corrupt tables */
        if (nlen + 5 < sizeof(names)) {
            names[nlen++] = ' ';
            memcpy(names + nlen, t->signature, 4);
            nlen += 4;
        }
        if (memcmp(t->signature, "FACP", 4) == 0) fadt = (const struct fadt *)t;
        else if (memcmp(t->signature, "APIC", 4) == 0) parse_madt((const struct madt *)t);
    }
    names[nlen] = '\0';
    kprintf("ATOS: acpi: revision %d, %s with %u tables:%s\n", acpi_revision,
            use_xsdt ? "XSDT" : "RSDT", count, names);

    if (fadt) {
        uint64_t dsdt_phys = fadt->dsdt;
        if (fadt->h.length >= offsetof(struct fadt, x_dsdt) + 8 && fadt->x_dsdt) dsdt_phys = fadt->x_dsdt;
        if (dsdt_phys) {
            const struct sdt_header *dsdt = map_table(dsdt_phys);
            if (checksum_ok(dsdt, dsdt->length)) find_s5(dsdt);
        }
        kprintf("ATOS: acpi: FADT: SCI irq %u, PM1a_CNT %#x, reset register %s, \\_S5 %s\n",
                fadt->sci_int, fadt->pm1a_cnt_blk,
                (fadt->flags & FADT_RESET_REG_SUP) ? "yes" : "no", s5_found ? "found" : "not found");
    }
    if (have_madt) {
        kprintf("ATOS: acpi: MADT: %u CPU(s), %u I/O APIC(s), %u IRQ override(s), LAPIC at %#lx\n",
                madt_info.cpu_count, madt_info.ioapic_count, madt_info.override_count,
                madt_info.lapic_addr);
    }
}

const struct acpi_madt_info *acpi_madt(void) {
    return have_madt ? &madt_info : NULL;
}

uint32_t acpi_isa_irq_to_gsi(uint8_t irq, uint16_t *flags) {
    *flags = 0;
    for (unsigned i = 0; have_madt && i < madt_info.override_count; i++) {
        if (madt_info.overrides[i].irq == irq) {
            *flags = madt_info.overrides[i].flags;
            return madt_info.overrides[i].gsi;
        }
    }
    return irq;
}

uint8_t acpi_rtc_century_register(void) {
    return fadt ? fadt->century : 0;
}

static void write_gas(const struct gas *g, uint8_t value) {
    if (g->space == GAS_IO) {
        outb((uint16_t)g->address, value);
    } else if (g->space == GAS_MEMORY) {
        volatile uint8_t *reg = vmm_map_phys(g->address, 1, VMM_NOCACHE);
        *reg = value;
    }
}

static void short_delay(void) {
    for (int i = 0; i < 100000; i++) io_wait();
}

void acpi_poweroff(void) {
    asm volatile("cli");
    if (fadt && s5_found && fadt->pm1a_cnt_blk) {
        uint16_t pm1a = (uint16_t)fadt->pm1a_cnt_blk;
        /* The PM registers only respond once the firmware is in ACPI mode. */
        if (!(inw(pm1a) & PM1_SCI_EN) && fadt->smi_cmd && fadt->acpi_enable) {
            outb((uint16_t)fadt->smi_cmd, fadt->acpi_enable);
            for (int i = 0; i < 300 && !(inw(pm1a) & PM1_SCI_EN); i++) short_delay();
        }
        outw(pm1a, (uint16_t)((inw(pm1a) & ~PM1_SLP_TYP(7)) | PM1_SLP_TYP(s5_typ_a) | PM1_SLP_EN));
        if (fadt->pm1b_cnt_blk) {
            uint16_t pm1b = (uint16_t)fadt->pm1b_cnt_blk;
            outw(pm1b, (uint16_t)((inw(pm1b) & ~PM1_SLP_TYP(7)) | PM1_SLP_TYP(s5_typ_b) | PM1_SLP_EN));
        }
        short_delay();
    }
    kprintf("ATOS: ACPI power-off failed, trying emulator ports\n");
    outw(0x604, 0x2000);  /* QEMU (q35 and newer i440fx) */
    outw(0xB004, 0x2000); /* Bochs, old QEMU */
    kprintf("ATOS: power-off failed; it is now safe to turn off the machine\n");
    for (;;) asm volatile("hlt");
}

void acpi_reboot(void) {
    asm volatile("cli");
    if (fadt && fadt->h.length >= offsetof(struct fadt, reset_value) + 1 &&
        (fadt->flags & FADT_RESET_REG_SUP)) {
        write_gas(&fadt->reset_reg, fadt->reset_value);
        short_delay();
    }
    /* 8042: pulse the CPU reset line once its input buffer is empty. */
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) io_wait();
    outb(0x64, 0xFE);
    short_delay();
    /* Last resort: with an empty IDT the next exception triple-faults. */
    static const struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) empty_idt = {0, 0};
    asm volatile("lidt %0; int3" : : "m"(empty_idt));
    for (;;) asm volatile("hlt");
}

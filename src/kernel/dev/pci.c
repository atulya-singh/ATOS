#include "pci.h"
#include "../lib/spinlock.h"
#include "../lib/io.h"
#include "../lib/kprintf.h"
#include <stddef.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_VENDOR_ID   0x00
#define PCI_COMMAND     0x04
#define PCI_CLASS_REV   0x08
#define PCI_HEADER_TYPE 0x0E
#define PCI_BAR0        0x10
#define PCI_IRQ_LINE    0x3C

#define PCI_COMMAND_IO     0x1
#define PCI_COMMAND_MEMORY 0x2
#define PCI_COMMAND_MASTER 0x4

/* Plenty for QEMU and typical hardware; extra functions are counted but
 * not recorded. */
#define MAX_PCI_DEVICES 64
static struct pci_device devices[MAX_PCI_DEVICES];
static unsigned device_count;

/* The address/data port pair is one shared register window, so a config
 * access must not be split by another CPU's (or an interrupt's) access. */
static struct spinlock config_lock;

static uint32_t config_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    uint64_t flags = spin_lock_irqsave(&config_lock);
    outl(PCI_CONFIG_ADDRESS, addr);
    uint32_t v = inl(PCI_CONFIG_DATA);
    spin_unlock_irqrestore(&config_lock, flags);
    return v;
}

static void config_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11)
                  | ((uint32_t)func << 8) | (offset & 0xFC);
    uint64_t flags = spin_lock_irqsave(&config_lock);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, value);
    spin_unlock_irqrestore(&config_lock, flags);
}

uint32_t pci_read32(const struct pci_device *d, uint8_t offset) {
    return config_read(d->bus, d->dev, d->func, offset);
}

uint16_t pci_read16(const struct pci_device *d, uint8_t offset) {
    return (uint16_t)(pci_read32(d, offset) >> ((offset & 2) * 8));
}

void pci_write32(const struct pci_device *d, uint8_t offset, uint32_t value) {
    config_write(d->bus, d->dev, d->func, offset, value);
}

void pci_write16(const struct pci_device *d, uint8_t offset, uint16_t value) {
    uint32_t v = pci_read32(d, offset);
    unsigned shift = (offset & 2) * 8;
    v = (v & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    pci_write32(d, offset, v);
}

uint64_t pci_bar(const struct pci_device *d, unsigned index, int *is_io) {
    uint8_t off = (uint8_t)(PCI_BAR0 + index * 4);
    uint32_t bar = pci_read32(d, off);
    if (bar & 1) {
        *is_io = 1;
        return bar & ~0x3u;
    }
    *is_io = 0;
    uint64_t addr = bar & ~0xFu;
    if (((bar >> 1) & 3) == 2) addr |= (uint64_t)pci_read32(d, (uint8_t)(off + 4)) << 32;
    return addr;
}

void pci_enable(const struct pci_device *d) {
    uint16_t cmd = pci_read16(d, PCI_COMMAND);
    pci_write16(d, PCI_COMMAND, cmd | PCI_COMMAND_IO | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
}

static const char *class_name(uint8_t class_code, uint8_t subclass) {
    switch (class_code) {
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "storage controller";
        }
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI-to-PCI bridge";
        default:   return "bridge";
        }
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "serial bus controller";
        }
    default:   return "other";
    }
}

static void probe_function(uint8_t bus, uint8_t dev, uint8_t func) {
    uint32_t id = config_read(bus, dev, func, PCI_VENDOR_ID);
    if ((id & 0xFFFF) == 0xFFFF) return;

    uint32_t class_rev = config_read(bus, dev, func, PCI_CLASS_REV);
    struct pci_device d = {
        .bus = bus, .dev = dev, .func = func,
        .vendor_id = (uint16_t)id,
        .device_id = (uint16_t)(id >> 16),
        .class_code = (uint8_t)(class_rev >> 24),
        .subclass = (uint8_t)(class_rev >> 16),
        .prog_if = (uint8_t)(class_rev >> 8),
        .header_type = (uint8_t)((config_read(bus, dev, func, 0x0C) >> 16) & 0x7F),
        .irq_line = (uint8_t)config_read(bus, dev, func, PCI_IRQ_LINE),
    };

    kprintf("ATOS: PCI %02x:%02x.%x %04x:%04x class %02x.%02x.%02x %s\n",
            bus, dev, func, d.vendor_id, d.device_id,
            d.class_code, d.subclass, d.prog_if, class_name(d.class_code, d.subclass));

    if (device_count < MAX_PCI_DEVICES) devices[device_count] = d;
    device_count++;
}

void pci_init(void) {
    /* Brute force over all 256 buses instead of walking bridges: it's a
     * few thousand port reads, done once, and immune to firmware leaving
     * bridge bus numbers in some state we didn't anticipate. */
    for (unsigned bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            uint32_t id = config_read((uint8_t)bus, dev, 0, PCI_VENDOR_ID);
            if ((id & 0xFFFF) == 0xFFFF) continue;
            uint8_t header = (uint8_t)(config_read((uint8_t)bus, dev, 0, 0x0C) >> 16);
            uint8_t nfuncs = (header & 0x80) ? 8 : 1;
            for (uint8_t func = 0; func < nfuncs; func++) probe_function((uint8_t)bus, dev, func);
        }
    }
    if (device_count > MAX_PCI_DEVICES) {
        kprintf("ATOS: PCI: %u functions found, only the first %u recorded\n",
                device_count, MAX_PCI_DEVICES);
        device_count = MAX_PCI_DEVICES;
    }
    kprintf("ATOS: PCI: %u functions enumerated\n", device_count);
}

unsigned pci_device_count(void) { return device_count; }

struct pci_device *pci_device_at(unsigned index) {
    return index < device_count ? &devices[index] : NULL;
}

struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (unsigned i = 0; i < device_count; i++) {
        if (devices[i].vendor_id == vendor_id && devices[i].device_id == device_id) return &devices[i];
    }
    return NULL;
}

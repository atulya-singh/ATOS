#pragma once
#include <stdint.h>

struct pci_device {
    uint8_t bus, dev, func;
    uint16_t vendor_id, device_id;
    uint8_t class_code, subclass, prog_if;
    uint8_t header_type; /* with the multi-function bit masked off */
    uint8_t irq_line;    /* legacy PIC line as programmed by firmware; 0xFF = none */
};

/* Enumerates every function on every bus through the legacy config
 * mechanism (ports 0xCF8/0xCFC) and logs what it finds. */
void pci_init(void);

unsigned pci_device_count(void);
struct pci_device *pci_device_at(unsigned index);
/* First device matching vendor:device, or NULL. */
struct pci_device *pci_find_device(uint16_t vendor_id, uint16_t device_id);

uint32_t pci_read32(const struct pci_device *d, uint8_t offset);
uint16_t pci_read16(const struct pci_device *d, uint8_t offset);
void pci_write32(const struct pci_device *d, uint8_t offset, uint32_t value);
void pci_write16(const struct pci_device *d, uint8_t offset, uint16_t value);

/* Decodes BAR `index` (type 0 headers). Returns the base address with the
 * type bits stripped (0 if unimplemented), and sets *is_io. 64-bit memory
 * BARs are combined with their upper half. */
uint64_t pci_bar(const struct pci_device *d, unsigned index, int *is_io);

/* Turns on I/O space, memory space, and bus mastering (DMA) decoding. */
void pci_enable(const struct pci_device *d);

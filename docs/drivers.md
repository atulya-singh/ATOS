# Device drivers

Code: `src/kernel/dev/`, `src/kernel/acpi/`.

| Driver | File | Notes |
|--------|------|-------|
| Serial (COM1) | `serial.c` | 38400 8N1, polled output; the primary debug log |
| Framebuffer console | `fbcon.c`, `font.S` | Text on Limine's framebuffer with the Terminus 8x16 font; also draws the red panic screen |
| Console | `console.c` | Mirrors output to serial + fbcon under `console_lock` |
| Keyboard | `keyboard.c` | PS/2 set 1 via IOAPIC IRQ 1; the IRQ pushes into a lock-free SPSC ring; readers take bytes raw |
| Timer | `timer.c` | Global tick count, advanced by the BSP's LAPIC timer (100 Hz) |
| RTC | `rtc.c` | CMOS clock, with the century register taken from the FADT |
| PCI | `pci.c` | Enumerates every bus/device/function through ports 0xCF8/0xCFC; decodes BARs; enables I/O, memory, and bus mastering |
| virtio-blk | `virtio_blk.c` | Legacy (transitional, device 0x1001) interface; one request in flight through a 64 KiB bounce buffer; disks appear as `vda`, `vdb`, ... |
| virtio-net | `virtio_net.c` | Legacy (device 0x1000); 64 receive and 32 transmit buffers; registers the interface with the network stack |
| ACPI | `acpi/acpi.c` | RSDP → RSDT/XSDT; MADT (CPUs, IOAPICs, overrides); FADT; `\_S5` for poweroff; reboot through the reset register, then the 8042, then a triple fault |

## virtio

Both virtio drivers use the legacy PCI interface: a register block in
I/O space at BAR0, with the spec's fixed split-ring layout in one
physically contiguous allocation. That interface needs no capability
parsing or MMIO mapping. QEMU exposes it with `disable-legacy=off`, which
`tools/run.sh` and the smoke test pass. The ring format is the same in
virtio 1.0, so moving to the modern interface would change only the
setup code.

**Polling.** Both drivers suppress device interrupts
(`VRING_AVAIL_F_NO_INTERRUPT`) and poll:

- virtio-blk yields while waiting for each request.
- virtio-net is polled by the network thread (see [net.md](net.md)).

Routing PCI INTx on q35 correctly needs the ACPI `_PRT`, which means an
AML interpreter. MSI-X is the likely next step.

## Adding a PCI driver

1. Walk `pci_device_at(i)` for your vendor/device IDs.
2. Map BARs: I/O ports directly, or MMIO through `vmm_map_phys(...,
   VMM_NOCACHE)`.
3. Call `pci_enable`.
4. Allocate DMA memory with `pmm_alloc_contiguous`, and reach it through
   `phys_to_virt`.
5. Call your init from `kmain_stage2` after `pci_init`.

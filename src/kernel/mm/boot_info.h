#pragma once
#include <limine.h>
#include <stdint.h>

/* Validates that Limine answered every request this kernel depends on.
 * Call once, early in kmain, before anything below is used. */
void boot_info_init(void);

struct limine_memmap_response *boot_info_memmap(void);
uint64_t boot_info_kernel_phys_base(void);
uint64_t boot_info_kernel_virt_base(void);

/* The kernel command line from limine.conf's `cmdline:` ("" if none). */
const char *boot_info_cmdline(void);

/* The single canonical physical<->virtual conversion, per the Limine HHDM
 * offset -- every subsystem uses these instead of hand-rolling one. */
void *phys_to_virt(uint64_t phys);
uint64_t virt_to_phys_hhdm(const void *hhdm_virt);

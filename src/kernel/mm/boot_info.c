#include "boot_info.h"
#include "../lib/kprintf.h"
#include <stddef.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_kernel_address_request kernel_address_request = {
    .id = LIMINE_KERNEL_ADDRESS_REQUEST,
    .revision = 0,
};

static uint64_t hhdm_offset;

void boot_info_init(void) {
    if (hhdm_request.response == NULL || memmap_request.response == NULL ||
        kernel_address_request.response == NULL) {
        kprintf("ATOS: fatal: Limine did not answer a required boot request\n");
        for (;;) asm volatile("cli; hlt");
    }
    hhdm_offset = hhdm_request.response->offset;
}

struct limine_memmap_response *boot_info_memmap(void) {
    return memmap_request.response;
}

uint64_t boot_info_kernel_phys_base(void) {
    return kernel_address_request.response->physical_base;
}

uint64_t boot_info_kernel_virt_base(void) {
    return kernel_address_request.response->virtual_base;
}

void *phys_to_virt(uint64_t phys) {
    return (void *)(phys + hhdm_offset);
}

uint64_t virt_to_phys_hhdm(const void *hhdm_virt) {
    return (uint64_t)hhdm_virt - hhdm_offset;
}

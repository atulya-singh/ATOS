#include "vmm.h"
#include "pmm.h"
#include "boot_info.h"
#include "../lib/kprintf.h"

#define PAGE_SIZE_2M   0x200000ULL
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_HUGE       (1ULL << 7)

/* Sized to comfortably survive vmm_init()'s own work plus everything else
 * that runs before a real per-task stack exists in Phase 3. */
#define KERNEL_STACK_SIZE (64 * 1024)
static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

static uint64_t *pml4;

extern __attribute__((noreturn)) void vmm_switch_and_continue(uint64_t cr3, uint64_t new_rsp,
                                                                void (*continuation)(void));

static inline uint64_t pml4_index(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t pdpt_index(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t pd_index(uint64_t v)   { return (v >> 21) & 0x1FF; }
static inline uint64_t pt_index(uint64_t v)   { return (v >> 12) & 0x1FF; }

/* Returns an HHDM pointer to the child table referenced by table[idx],
 * allocating and zeroing a fresh page table if it isn't present yet.
 * Non-leaf entries are always present+writable+user -- permissions are
 * enforced at the leaf (see map_2m/map_4k), same as every other x86 OS. */
static uint64_t *walk(uint64_t *table, uint64_t idx) {
    if (!(table[idx] & VMM_PRESENT)) {
        uint64_t phys = pmm_alloc_page();
        table[idx] = phys | VMM_PRESENT | VMM_WRITABLE | VMM_USER;
    }
    return phys_to_virt(table[idx] & PTE_ADDR_MASK);
}

static void map_2m(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pdpt = walk(pml4, pml4_index(virt));
    uint64_t *pd = walk(pdpt, pdpt_index(virt));
    pd[pd_index(virt)] = (phys & ~(PAGE_SIZE_2M - 1)) | flags | PTE_HUGE;
}

static void map_4k(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pdpt = walk(pml4, pml4_index(virt));
    uint64_t *pd = walk(pdpt, pdpt_index(virt));
    uint64_t *pt = walk(pd, pd_index(virt));
    pt[pt_index(virt)] = (phys & ~(PAGE_SIZE - 1)) | flags;
}

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    map_4k(virt, phys, flags);
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    uint64_t end = virt + size;
    while (virt < end) {
        uint64_t remaining = end - virt;
        if (remaining >= PAGE_SIZE_2M && (virt % PAGE_SIZE_2M) == 0 && (phys % PAGE_SIZE_2M) == 0) {
            map_2m(virt, phys, flags);
            virt += PAGE_SIZE_2M;
            phys += PAGE_SIZE_2M;
        } else {
            map_4k(virt, phys, flags);
            virt += PAGE_SIZE;
            phys += PAGE_SIZE;
        }
    }
}

int vmm_translate(uint64_t virt, uint64_t *out_phys) {
    if (!(pml4[pml4_index(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pdpt = phys_to_virt(pml4[pml4_index(virt)] & PTE_ADDR_MASK);

    if (!(pdpt[pdpt_index(virt)] & VMM_PRESENT)) return 0;
    uint64_t *pd = phys_to_virt(pdpt[pdpt_index(virt)] & PTE_ADDR_MASK);

    if (!(pd[pd_index(virt)] & VMM_PRESENT)) return 0;
    if (pd[pd_index(virt)] & PTE_HUGE) {
        *out_phys = (pd[pd_index(virt)] & PTE_ADDR_MASK & ~(PAGE_SIZE_2M - 1)) | (virt & (PAGE_SIZE_2M - 1));
        return 1;
    }
    uint64_t *pt = phys_to_virt(pd[pd_index(virt)] & PTE_ADDR_MASK);

    if (!(pt[pt_index(virt)] & VMM_PRESENT)) return 0;
    *out_phys = (pt[pt_index(virt)] & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
    return 1;
}

void vmm_unmap(uint64_t virt) {
    if (!(pml4[pml4_index(virt)] & VMM_PRESENT)) return;
    uint64_t *pdpt = phys_to_virt(pml4[pml4_index(virt)] & PTE_ADDR_MASK);
    if (!(pdpt[pdpt_index(virt)] & VMM_PRESENT)) return;
    uint64_t *pd = phys_to_virt(pdpt[pdpt_index(virt)] & PTE_ADDR_MASK);
    if (!(pd[pd_index(virt)] & VMM_PRESENT)) return;

    if (pd[pd_index(virt)] & PTE_HUGE) {
        pd[pd_index(virt)] = 0;
    } else {
        uint64_t *pt = phys_to_virt(pd[pd_index(virt)] & PTE_ADDR_MASK);
        pt[pt_index(virt)] = 0;
    }
    asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

/* PTE bit 63 (NX) is a *reserved* bit -- not merely a no-op -- until
 * EFER.NXE is set, so setting it on any mapping before this runs would
 * fault. Must run before the first NX mapping is installed. */
static void enable_nx(void) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    lo |= (1 << 11);
    asm volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(0xC0000080));
}

__attribute__((noreturn)) void vmm_init(void (*continuation)(void)) {
    enable_nx();

    uint64_t pml4_phys = pmm_alloc_page();
    pml4 = phys_to_virt(pml4_phys);

    /* Replicate Limine's HHDM: every reported physical range, direct-mapped
     * at hhdm_offset + phys, not executable. This is what every phys_to_virt
     * pointer (PMM bitmap, page tables themselves, the future heap's
     * backing pages) keeps depending on after we take over CR3. */
    struct limine_memmap_response *memmap = boot_info_memmap();
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_BAD_MEMORY) continue;
        uint64_t base = e->base & ~(PAGE_SIZE - 1);
        uint64_t top = (e->base + e->length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        vmm_map_range((uint64_t)phys_to_virt(base), base, top - base,
                      VMM_PRESENT | VMM_WRITABLE | VMM_NX);
    }

    /* Map the kernel image. First a blanket RW mapping covering everything
     * from kernel_image_start (which includes .limine_requests -- boot_info
     * re-reads Limine's response pointers from there) through kernel_end,
     * then tighten .text to R-X and .rodata to R-- on top of it. */
    uint64_t kphys = boot_info_kernel_phys_base();
    uint64_t kvirt = boot_info_kernel_virt_base();

    extern char kernel_image_start[], kernel_end[];
    extern char kernel_text_start[], kernel_text_end[];
    extern char kernel_rodata_start[], kernel_rodata_end[];

    uint64_t image_size = (uint64_t)kernel_end - (uint64_t)kernel_image_start;
    vmm_map_range((uint64_t)kernel_image_start,
                  kphys + ((uint64_t)kernel_image_start - kvirt),
                  image_size, VMM_PRESENT | VMM_WRITABLE | VMM_NX);

    uint64_t text_size = (uint64_t)kernel_text_end - (uint64_t)kernel_text_start;
    vmm_map_range((uint64_t)kernel_text_start,
                  kphys + ((uint64_t)kernel_text_start - kvirt),
                  text_size, VMM_PRESENT); /* R-X: not writable, no NX */

    uint64_t rodata_size = (uint64_t)kernel_rodata_end - (uint64_t)kernel_rodata_start;
    vmm_map_range((uint64_t)kernel_rodata_start,
                  kphys + ((uint64_t)kernel_rodata_start - kvirt),
                  rodata_size, VMM_PRESENT | VMM_NX); /* R--: not writable */

    kprintf("ATOS: VMM: page tables built (HHDM + W^X kernel image)\n");

    uint64_t new_rsp = (uint64_t)(kernel_stack + KERNEL_STACK_SIZE) - 8;
    vmm_switch_and_continue(pml4_phys, new_rsp, continuation);
}

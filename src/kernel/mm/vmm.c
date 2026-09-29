#include "vmm.h"
#include "pmm.h"
#include "boot_info.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include <stddef.h>

#define PAGE_SIZE_2M   0x200000ULL
#define PTE_ADDR_MASK  0x000FFFFFFFFFF000ULL
#define PTE_HUGE       (1ULL << 7)

/* Sized to comfortably survive vmm_init()'s own work plus everything else
 * that runs before a real per-task stack exists in Phase 3. */
#define KERNEL_STACK_SIZE (64 * 1024)
static uint8_t kernel_stack[KERNEL_STACK_SIZE] __attribute__((aligned(16)));

static uint64_t *pml4; /* the kernel's; its upper half is shared by every address space */
static uint64_t kernel_pml4_phys;

#define USER_HALF_END 0x0000800000000000ULL

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

static void map_2m(uint64_t *root, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pdpt = walk(root, pml4_index(virt));
    uint64_t *pd = walk(pdpt, pdpt_index(virt));
    pd[pd_index(virt)] = (phys & ~(PAGE_SIZE_2M - 1)) | flags | PTE_HUGE;
}

static void map_4k(uint64_t *root, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pdpt = walk(root, pml4_index(virt));
    uint64_t *pd = walk(pdpt, pdpt_index(virt));
    uint64_t *pt = walk(pd, pd_index(virt));
    pt[pt_index(virt)] = (phys & ~(PAGE_SIZE - 1)) | flags;
}

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    map_4k(pml4, virt, phys, flags);
}

void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags) {
    uint64_t end = virt + size;
    while (virt < end) {
        uint64_t remaining = end - virt;
        if (remaining >= PAGE_SIZE_2M && (virt % PAGE_SIZE_2M) == 0 && (phys % PAGE_SIZE_2M) == 0) {
            map_2m(pml4, virt, phys, flags);
            virt += PAGE_SIZE_2M;
            phys += PAGE_SIZE_2M;
        } else {
            map_4k(pml4, virt, phys, flags);
            virt += PAGE_SIZE;
            phys += PAGE_SIZE;
        }
    }
}

void vmm_prealloc_tables(uint64_t virt, uint64_t size) {
    for (uint64_t v = virt & ~(PAGE_SIZE_2M - 1); v < virt + size; v += PAGE_SIZE_2M) {
        uint64_t *pdpt = walk(pml4, pml4_index(v));
        uint64_t *pd = walk(pdpt, pdpt_index(v));
        walk(pd, pd_index(v));
    }
}

/* Returns the leaf entry mapping `virt` in `root` (a PT entry, or a PD
 * entry for a 2 MiB page), or NULL if some level along the way is absent.
 * If `all_user` is non-NULL it reports whether every level permits ring 3. */
static uint64_t *lookup(uint64_t *root, uint64_t virt, int *all_user) {
    uint64_t user = VMM_USER;
    uint64_t *table = root;
    uint64_t idx[3] = {pml4_index(virt), pdpt_index(virt), pd_index(virt)};
    for (int level = 0; level < 3; level++) {
        uint64_t e = table[idx[level]];
        if (!(e & VMM_PRESENT)) return NULL;
        user &= e;
        if (level == 2 && (e & PTE_HUGE)) {
            if (all_user) *all_user = (user & VMM_USER) != 0;
            return &table[idx[level]];
        }
        table = phys_to_virt(e & PTE_ADDR_MASK);
    }
    uint64_t *pte = &table[pt_index(virt)];
    if (!(*pte & VMM_PRESENT)) return NULL;
    if (all_user) *all_user = (user & *pte & VMM_USER) != 0;
    return pte;
}

int vmm_translate(uint64_t virt, uint64_t *out_phys) {
    uint64_t *pte = lookup(pml4, virt, NULL);
    if (!pte) return 0;
    if (*pte & PTE_HUGE)
        *out_phys = (*pte & PTE_ADDR_MASK & ~(PAGE_SIZE_2M - 1)) | (virt & (PAGE_SIZE_2M - 1));
    else
        *out_phys = (*pte & PTE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
    return 1;
}

void *vmm_map_phys(uint64_t phys, uint64_t len, uint64_t flags) {
    uint64_t first = phys & ~(PAGE_SIZE - 1);
    for (uint64_t p = first; p < phys + len; p += PAGE_SIZE) {
        uint64_t virt = (uint64_t)phys_to_virt(p), unused;
        /* Checked per page: an existing 2 MiB HHDM page must not be walked
         * into as if it were a page table. */
        if (!vmm_translate(virt, &unused)) {
            map_4k(pml4, virt, p, VMM_PRESENT | VMM_WRITABLE | VMM_NX | flags);
        }
    }
    return phys_to_virt(phys);
}

void vmm_unmap(uint64_t virt) {
    uint64_t *pte = lookup(pml4, virt, NULL);
    if (!pte) return;
    *pte = 0;
    asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
}

uint64_t vmm_kernel_cr3(void) {
    return kernel_pml4_phys;
}

uint64_t vmm_create_address_space(void) {
    uint64_t phys = pmm_alloc_page();
    if (!phys) return 0;
    uint64_t *root = phys_to_virt(phys);
    /* Lower half stays empty (pmm pages come back zeroed); upper half points
     * at the kernel's own PDPTs, which vmm_init() pre-allocated so that no
     * later kernel mapping can create a PML4 entry this copy would miss. */
    for (int i = 256; i < 512; i++) root[i] = pml4[i];
    return phys;
}

void vmm_map_user(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags) {
    map_4k(phys_to_virt(cr3), virt, phys, flags | VMM_USER);
}

uint64_t vmm_user_lookup(uint64_t cr3, uint64_t virt, uint64_t *flags) {
    if (virt >= USER_HALF_END) return 0;
    uint64_t *pte = lookup(phys_to_virt(cr3), virt, NULL);
    if (!pte || (*pte & PTE_HUGE)) return 0;
    if (flags) *flags = *pte & ~PTE_ADDR_MASK;
    return *pte & PTE_ADDR_MASK;
}

uint64_t vmm_unmap_user(uint64_t cr3, uint64_t virt) {
    if (virt >= USER_HALF_END) return 0;
    uint64_t *pte = lookup(phys_to_virt(cr3), virt, NULL);
    if (!pte || (*pte & PTE_HUGE)) return 0;
    uint64_t phys = *pte & PTE_ADDR_MASK;
    *pte = 0;
    uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if ((current_cr3 & PTE_ADDR_MASK) == cr3) asm volatile("invlpg (%0)" ::"r"(virt) : "memory");
    return phys;
}

void vmm_destroy_address_space(uint64_t cr3) {
    uint64_t *root = phys_to_virt(cr3);
    /* Only the lower half belongs to the process; the upper half is the
     * shared kernel, and freeing it here would pull it out from under
     * every other address space. */
    for (int i = 0; i < 256; i++) {
        if (!(root[i] & VMM_PRESENT)) continue;
        uint64_t *pdpt = phys_to_virt(root[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & VMM_PRESENT)) continue;
            uint64_t *pd = phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_PRESENT)) continue;
                uint64_t *pt = phys_to_virt(pd[k] & PTE_ADDR_MASK);
                for (int l = 0; l < 512; l++) {
                    if (pt[l] & VMM_PRESENT) pmm_free_page(pt[l] & PTE_ADDR_MASK);
                }
                pmm_free_page(pd[k] & PTE_ADDR_MASK);
            }
            pmm_free_page(pdpt[j] & PTE_ADDR_MASK);
        }
        pmm_free_page(root[i] & PTE_ADDR_MASK);
    }
    pmm_free_page(cr3);
}

uint64_t vmm_clone_address_space(uint64_t src_cr3) {
    uint64_t dst_cr3 = vmm_create_address_space();
    if (!dst_cr3) return 0;
    uint64_t *src = phys_to_virt(src_cr3);

    for (int i = 0; i < 256; i++) {
        if (!(src[i] & VMM_PRESENT)) continue;
        uint64_t *pdpt = phys_to_virt(src[i] & PTE_ADDR_MASK);
        for (int j = 0; j < 512; j++) {
            if (!(pdpt[j] & VMM_PRESENT)) continue;
            uint64_t *pd = phys_to_virt(pdpt[j] & PTE_ADDR_MASK);
            for (int k = 0; k < 512; k++) {
                if (!(pd[k] & VMM_PRESENT)) continue;
                uint64_t *pt = phys_to_virt(pd[k] & PTE_ADDR_MASK);
                for (int l = 0; l < 512; l++) {
                    if (!(pt[l] & VMM_PRESENT)) continue;
                    uint64_t copy = pmm_alloc_page();
                    if (!copy) {
                        vmm_destroy_address_space(dst_cr3);
                        return 0;
                    }
                    memcpy(phys_to_virt(copy), phys_to_virt(pt[l] & PTE_ADDR_MASK), PAGE_SIZE);
                    uint64_t va = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                  ((uint64_t)k << 21) | ((uint64_t)l << 12);
                    vmm_map_user(dst_cr3, va, copy, pt[l] & ~PTE_ADDR_MASK);
                }
            }
        }
    }
    return dst_cr3;
}

int vmm_user_range_ok(uint64_t cr3, uint64_t addr, uint64_t len, int need_write) {
    if (len == 0) return 1;
    if (addr + len < addr || addr + len > USER_HALF_END) return 0;
    uint64_t *root = phys_to_virt(cr3);
    for (uint64_t page = addr & ~(PAGE_SIZE - 1); page < addr + len; page += PAGE_SIZE) {
        int all_user;
        uint64_t *pte = lookup(root, page, &all_user);
        if (!pte || !all_user) return 0;
        if (need_write && !(*pte & VMM_WRITABLE)) return 0;
    }
    return 1;
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
    kernel_pml4_phys = pml4_phys;

    /* Pre-populate every kernel-half PML4 slot (256 pages, 1 MiB). Process
     * address spaces copy these 256 entries once, at creation; with them
     * fixed up front, kernel mappings made afterwards (heap, task stacks)
     * are visible in every address space with no synchronization. No USER
     * bit at this level: ring 3 can never reach anything beneath it. */
    for (int i = 256; i < 512; i++) {
        pml4[i] = pmm_alloc_page() | VMM_PRESENT | VMM_WRITABLE;
    }

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

#pragma once
#include <stdint.h>

#define VMM_PRESENT  (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER     (1ULL << 2)
#define VMM_NOCACHE  ((1ULL << 3) | (1ULL << 4)) /* PWT + PCD: for MMIO registers */
#define VMM_NX       (1ULL << 63) /* only valid once EFER.NXE is set; see vmm_init */

/* Builds a fresh set of page tables (HHDM + the kernel image, with correct
 * per-section permissions) and switches to them, relocating onto a
 * kernel-owned stack in the same move -- see vmm_switch_and_continue in
 * vmm_asm.S for why the stack has to move too. Never returns; `continuation`
 * is where execution resumes on the new stack/tables. */
__attribute__((noreturn)) void vmm_init(void (*continuation)(void));

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_map_range(uint64_t virt, uint64_t phys, uint64_t size, uint64_t flags);
void vmm_unmap(uint64_t virt);

/* Makes [phys, phys+len) reachable through the HHDM and returns the HHDM
 * pointer. Most RAM already is (vmm_init maps every memmap range); this is
 * for what isn't, like device registers (pass VMM_NOCACHE) or firmware
 * tables outside the memmap. Pages already mapped are left alone. */
void *vmm_map_phys(uint64_t phys, uint64_t len, uint64_t flags);
int vmm_translate(uint64_t virt, uint64_t *out_phys);

/* Allocates the intermediate tables covering [virt, virt+size) in the
 * kernel's address space without mapping any leaves, so that later
 * map/unmap cycles there (e.g. task stacks) never allocate or leak tables. */
void vmm_prealloc_tables(uint64_t virt, uint64_t size);

/* --- per-process address spaces (identified by their PML4's phys addr) --- */

uint64_t vmm_kernel_cr3(void);
/* New PML4 sharing the kernel half; returns 0 if out of memory. */
uint64_t vmm_create_address_space(void);
/* Maps one 4 KiB user page (VMM_USER is added implicitly). */
void vmm_map_user(uint64_t cr3, uint64_t virt, uint64_t phys, uint64_t flags);
/* Physical page mapped at user address `virt` in `cr3` (0 if none), and
 * optionally its PTE flag bits. */
uint64_t vmm_user_lookup(uint64_t cr3, uint64_t virt, uint64_t *flags);
/* Removes the mapping at `virt` and returns the page it pointed to (0 if
 * none); freeing that page is the caller's call. */
uint64_t vmm_unmap_user(uint64_t cr3, uint64_t virt);
/* Deep copy of the lower half of `src_cr3` (for fork): every user page
 * duplicated, same addresses and permissions. Returns 0 if out of memory. */
uint64_t vmm_clone_address_space(uint64_t src_cr3);
/* Frees every lower-half page and table, then the PML4 itself. The address
 * space must not be the one currently loaded in CR3. */
void vmm_destroy_address_space(uint64_t cr3);
/* True if every byte of [addr, addr+len) is mapped user-accessible (and
 * writable, if requested) in `cr3` -- how syscalls vet user pointers
 * before the kernel dereferences them. */
int vmm_user_range_ok(uint64_t cr3, uint64_t addr, uint64_t len, int need_write);

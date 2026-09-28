#pragma once
#include <stdint.h>

#define VMM_PRESENT  (1ULL << 0)
#define VMM_WRITABLE (1ULL << 1)
#define VMM_USER     (1ULL << 2)
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
int vmm_translate(uint64_t virt, uint64_t *out_phys);

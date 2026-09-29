#include "fpu.h"
#include "../../lib/string.h"

#define CR0_MP (1ULL << 1) /* WAIT/FWAIT honors TS */
#define CR0_EM (1ULL << 2) /* x87 emulation: must be off */
#define CR0_TS (1ULL << 3) /* task switched: we switch eagerly, so off */
#define CR0_NE (1ULL << 5) /* x87 errors raise #MF, not the legacy IRQ 13 */
#define CR4_OSFXSR     (1ULL << 9)  /* fxsave/fxrstor and SSE */
#define CR4_OSXMMEXCPT (1ULL << 10) /* unmasked SSE errors raise #XM */

static struct fpu_state clean_state;
static int have_clean_state;

void fpu_init_cpu(void) {
    uint64_t cr0, cr4;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 & ~(CR0_EM | CR0_TS)) | CR0_MP | CR0_NE;
    asm volatile("mov %0, %%cr0" : : "r"(cr0));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    asm volatile("mov %0, %%cr4" : : "r"(cr4 | CR4_OSFXSR | CR4_OSXMMEXCPT));

    uint32_t mxcsr = 0x1F80;
    asm volatile("fninit; ldmxcsr %0" : : "m"(mxcsr));
    if (!have_clean_state) {
        fpu_save(&clean_state);
        have_clean_state = 1;
    }
}

void fpu_init_state(struct fpu_state *s) {
    memcpy(s, &clean_state, sizeof(*s));
}

void fpu_reset(void) {
    fpu_restore(&clean_state);
}

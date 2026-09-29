#pragma once
#include <stdint.h>

/* x87/SSE state for user programs. The kernel itself is built without
 * FPU/SSE instructions, so the registers only ever hold the current
 * task's user state: the scheduler saves it with fxsave on the way out
 * of a task and restores it on the way in (see schedule()), and syscalls
 * and interrupts leave it alone. */

/* The 512-byte FXSAVE image; the instructions need 16-byte alignment. */
struct fpu_state {
    uint8_t data[512];
} __attribute__((aligned(16)));

/* Turns on the FPU and SSE on the calling CPU (CR0.MP/NE, CR4.OSFXSR and
 * OSXMMEXCPT). The first call, on the BSP, also records the clean state
 * new tasks start from. */
void fpu_init_cpu(void);

/* Fills `s` with the state a fresh program starts in (FNINIT defaults,
 * MXCSR 0x1F80: every exception masked, round to nearest). */
void fpu_init_state(struct fpu_state *s);

/* Loads the fresh-program state into this CPU's registers (exec). */
void fpu_reset(void);

static inline void fpu_save(struct fpu_state *s) {
    asm volatile("fxsave64 %0" : "=m"(*s));
}

static inline void fpu_restore(const struct fpu_state *s) {
    asm volatile("fxrstor64 %0" : : "m"(*s));
}

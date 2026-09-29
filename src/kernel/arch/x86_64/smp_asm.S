.section .text

/* void smp_enter_kernel(uint64_t cr3, uint64_t rsp, void (*fn)(struct cpu *), struct cpu *c)
 *
 * An AP's move from Limine's page tables and stack onto the kernel's: the
 * same one-way hop as vmm_switch_and_continue, plus an argument. The new
 * stack top is 16-byte aligned, so `call` leaves fn the alignment the ABI
 * promises. fn never returns. */
.global smp_enter_kernel
.type smp_enter_kernel, @function
smp_enter_kernel:
    mov %rdi, %cr3
    mov %rsi, %rsp
    xor %ebp, %ebp
    mov %rcx, %rdi
    call *%rdx
    ud2

.section .note.GNU-stack,"",@progbits

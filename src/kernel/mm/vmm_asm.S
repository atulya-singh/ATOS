.section .text

/* void vmm_switch_and_continue(uint64_t cr3, uint64_t new_rsp, void (*continuation)(void))
 *
 * Loads the new page tables and jumps onto a stack backed by them, in one
 * move. We can't just load %cr3 and carry on: Limine's own boot stack has
 * no guaranteed presence in the page tables we just built, so the very
 * next stack access after switching %cr3 alone could fault. Landing on
 * kernel_stack (part of the kernel image, which vmm_init() explicitly
 * mapped) sidesteps that entirely. This never returns to its caller --
 * the old stack, and everything on it, is abandoned. */
.global vmm_switch_and_continue
.type vmm_switch_and_continue, @function
vmm_switch_and_continue:
    mov %rdi, %cr3
    mov %rsi, %rsp
    xor %rbp, %rbp
    jmp *%rdx

.section .note.GNU-stack,"",@progbits

.section .text

/* void context_switch(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3)
 *
 * Saves the callee-saved registers on the current kernel stack, parks
 * %rsp in *old_rsp, and resumes the other task from its own saved stack.
 * Caller-saved registers need no saving: the C caller already assumes
 * this call clobbers them. Everything else a task owns (user registers,
 * RFLAGS, RIP) sits further up its kernel stack, in the interrupt or
 * syscall frame it was switched out from, and comes back when that frame
 * unwinds.
 *
 * CR3 is loaded only when it actually changes, since every write flushes
 * the TLB. Switching it while still on the old stack is safe: all kernel
 * stacks live in the kernel half, which every address space shares. */
.global context_switch
.type context_switch, @function
context_switch:
    push %rbp
    push %rbx
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, (%rdi)

    mov %cr3, %rax
    cmp %rax, %rdx
    je 1f
    mov %rdx, %cr3
1:
    mov %rsi, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbx
    pop %rbp
    ret

/* The first "return" of a brand-new task lands here. task_create_kernel
 * pre-loads %rbx with the entry function and %r12 with its argument,
 * through the register slots context_switch pops. The task starts with
 * interrupts disabled (schedule() always runs that way), so it re-enables
 * them before running any code of its own. */
.global task_trampoline
.type task_trampoline, @function
task_trampoline:
    sti
    mov %r12, %rdi
    call *%rbx
    xor %edi, %edi
    call task_exit /* noreturn */

/* First switch-in of a forked child (see task_fork): %rsp points at a copy
 * of the parent's struct registers syscall frame. Unwind it the way
 * isr_stubs.S's COMMON_STUB does, and iretq into user mode. */
.global fork_return
.type fork_return, @function
fork_return:
    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    add $16, %rsp /* int_no + err_code */
    iretq

/* void jump_to_user(uint64_t rip, uint64_t rsp) -- drops to ring 3 by
 * building the interrupt-return frame the CPU would have pushed had ring 3
 * been interrupted, then running iretq on it. The data segments are
 * loaded with the user selector (RPL 3), and every general-purpose
 * register is zeroed so no kernel values leak into user mode. */
.global jump_to_user
.type jump_to_user, @function
jump_to_user:
    cli
    mov $0x23, %ax
    mov %ax, %ds
    mov %ax, %es

    push $0x23      /* ss: user data | RPL 3 */
    push %rsi       /* rsp */
    push $0x202     /* rflags: IF set, so user mode stays preemptible */
    push $0x1b      /* cs: user code | RPL 3 */
    push %rdi       /* rip */

    xor %eax, %eax
    xor %ebx, %ebx
    xor %ecx, %ecx
    xor %edx, %edx
    xor %esi, %esi
    xor %edi, %edi
    xor %ebp, %ebp
    xor %r8d, %r8d
    xor %r9d, %r9d
    xor %r10d, %r10d
    xor %r11d, %r11d
    xor %r12d, %r12d
    xor %r13d, %r13d
    xor %r14d, %r14d
    xor %r15d, %r15d
    iretq

.section .note.GNU-stack,"",@progbits

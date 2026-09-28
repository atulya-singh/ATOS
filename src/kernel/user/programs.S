/* Built-in ring-3 test programs, until the Phase 5 ELF loader can pull
 * real binaries from the initrd. Each one is a position-independent blob.
 * It sits in the kernel's .rodata only as a byte image: task_create_user
 * copies it into user pages at USER_CODE_BASE, and it runs from there.
 * Syscall ABI: see src/kernel/sys/syscall.h. */

.section .rodata

/* Checks that a kernel pointer gets rejected with -EFAULT, then prints a
 * greeting three times, yielding between prints, and exits with 42. */
.global user_hello_start, user_hello_end
user_hello_start:
    mov $0, %eax                      /* SYS_WRITE */
    mov $1, %edi
    movabs $0xffffffff80000000, %rsi  /* kernel image: must be refused */
    mov $16, %edx
    int $0x80
    cmp $-14, %rax                    /* -EFAULT */
    jne 1f
    lea efault_msg(%rip), %rsi
    lea efault_msg_end(%rip), %rdx
    sub %rsi, %rdx
    mov $0, %eax
    mov $1, %edi
    int $0x80
1:
    mov $3, %r12d
2:
    mov $0, %eax                      /* SYS_WRITE */
    mov $1, %edi
    lea hello_msg(%rip), %rsi
    lea hello_msg_end(%rip), %rdx
    sub %rsi, %rdx
    int $0x80
    mov $2, %eax                      /* SYS_YIELD */
    int $0x80
    dec %r12d                         /* callee state survives the syscall */
    jnz 2b

    mov $1, %eax                      /* SYS_EXIT */
    mov $42, %edi
    int $0x80
    ud2                               /* unreachable */
efault_msg:
    .ascii "user: kernel pointer rejected with -EFAULT\n"
efault_msg_end:
hello_msg:
    .ascii "user: hello from ring 3!\n"
hello_msg_end:
user_hello_end:

/* Reads kernel memory directly. With no USER bit on those pages, this
 * #PFs, and only this task should die. */
.global user_fault_start, user_fault_end
user_fault_start:
    movabs $0xffffffff80000000, %rax
    mov (%rax), %rax
    ud2
user_fault_end:

/* Burns CPU in ring 3 with no syscalls in the loop, so the only way off
 * the CPU is a timer IRQ taken in user mode. That is the path that depends
 * on TSS.rsp0 pointing at this task's kernel stack. Exits with 7. */
.global user_spin_start, user_spin_end
user_spin_start:
    mov $300000000, %ecx
1:
    dec %ecx
    jnz 1b
    mov $1, %eax                      /* SYS_EXIT */
    mov $7, %edi
    int $0x80
    ud2
user_spin_end:

.section .note.GNU-stack,"",@progbits

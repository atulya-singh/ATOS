/* Process entry. The kernel starts us with %rsp -> argc, argv[], NULL
 * (see build_stack in src/kernel/proc/process.c), 16-byte aligned. */
.section .text._start, "ax"
.global _start
.type _start, @function
_start:
    xor %ebp, %ebp          /* marks the outermost frame for debuggers */
    mov (%rsp), %rdi        /* argc */
    lea 8(%rsp), %rsi       /* argv */
    call main
    mov %eax, %edi
    call exit               /* flushes nothing yet, then _exit */
    ud2

.section .note.GNU-stack,"",@progbits

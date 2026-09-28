.section .text

/* Vectors that don't push a CPU error code get a fake 0 pushed in its
 * place, so isr_common_stub sees the same stack shape either way. */
.macro ISR_NOERR num
.global isr\num
.type isr\num, @function
isr\num:
    push $0
    push $\num
    jmp isr_common_stub
.endm

.macro ISR_ERR num
.global isr\num
.type isr\num, @function
isr\num:
    push $\num
    jmp isr_common_stub
.endm

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31

.macro IRQ num, vec
.global irq\num
.type irq\num, @function
irq\num:
    push $0
    push $\vec
    jmp irq_common_stub
.endm

IRQ 0,  32
IRQ 1,  33
IRQ 2,  34
IRQ 3,  35
IRQ 4,  36
IRQ 5,  37
IRQ 6,  38
IRQ 7,  39
IRQ 8,  40
IRQ 9,  41
IRQ 10, 42
IRQ 11, 43
IRQ 12, 44
IRQ 13, 45
IRQ 14, 46
IRQ 15, 47

.extern isr_handler
.extern irq_handler

/* Pushes struct registers (see idt.h) in the exact field order, calls the
 * C handler with %rsp (post-push) as its single argument, then restores
 * and irets. %rsp is realigned to 16 bytes across the call since the CPU
 * doesn't guarantee stack alignment at the point an interrupt lands. */
.macro COMMON_STUB name, handler
\name:
    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    mov %rsp, %rdi
    mov %rsp, %rbp
    and $-16, %rsp
    call \handler
    mov %rbp, %rsp

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

    add $16, %rsp /* discard int_no + err_code */
    iretq
.endm

COMMON_STUB isr_common_stub, isr_handler
COMMON_STUB irq_common_stub, irq_handler

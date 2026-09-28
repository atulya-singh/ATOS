.section .text

/* void gdt_flush(uint64_t gdtr_addr) -- load the GDTR, then reload every
 * segment register. CS can't be reloaded with a plain mov, so we push a
 * far-return frame (selector + target RIP) and use lretq to reload it. */
.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    lgdt (%rdi)

    push $0x08          /* kernel code selector */
    lea 1f(%rip), %rax
    push %rax
    lretq
1:
    mov $0x10, %ax      /* kernel data selector */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ret

/* void tss_flush(void) -- load the task register with the TSS selector. */
.global tss_flush
.type tss_flush, @function
tss_flush:
    mov $0x28, %ax
    ltr %ax
    ret

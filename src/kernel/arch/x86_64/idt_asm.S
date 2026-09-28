.section .text

/* void idt_flush(uint64_t idtr_addr) */
.global idt_flush
.type idt_flush, @function
idt_flush:
    lidt (%rdi)
    ret

.section .note.GNU-stack,"",@progbits

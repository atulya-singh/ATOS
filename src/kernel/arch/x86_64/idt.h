#pragma once
#include <stdint.h>

/* Layout matches exactly what isr_stubs.S pushes onto the stack, read back
 * as a struct pointer passed in %rdi. See the push/pop order in that file
 * before changing either side of this contract. */
struct registers {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags;
} __attribute__((packed));

void idt_init(void);

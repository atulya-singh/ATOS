KERNEL := kernel.elf

CC := gcc
LD := ld

# -ffreestanding: Don't assume standard libraries exist (no main, no printf)
# -mno-red-zone: Essential for 64-bit interrupts so the CPU doesn't corrupt stack data
# -mcmodel=kernel: Puts kernel code in the higher-half virtual address space
CFLAGS := -Wall -Wextra -std=gnu11 \
          -ffreestanding \
          -fno-stack-protector \
          -fno-stack-check \
          -fno-PIC \
          -m64 \
          -march=x86-64 \
          -mno-red-zone \
          -mcmodel=kernel \
          -I src

LDFLAGS := -m elf_x86_64 \
           -nostdlib \
           -static \
           -T linker.ld

# Gather all C files inside src/
C_FILES := $(shell find src -type f -name '*.c')
OBJ_FILES := $(C_FILES:.c=.o)

.PHONY: all clean

all: $(KERNEL)

$(KERNEL): $(OBJ_FILES)
	$(LD) $(LDFLAGS) $(OBJ_FILES) -o $@

# Compile individual C source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ_FILES) $(KERNEL)
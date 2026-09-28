KERNEL := kernel.elf

# Overridable so CI (native x86_64 Linux) can use plain cc/ld while
# dev.sh's Docker container (arm64 host) points these at the
# x86_64-linux-gnu cross toolchain instead.
CC ?= cc
LD ?= ld

# -ffreestanding: don't assume standard libraries exist (no main, no printf);
#   this also implies -fno-builtin, so our libk memcpy/memset can't get
#   miscompiled into a recursive call to themselves.
# -mno-red-zone: essential once interrupts are involved, so the CPU pushing
#   a frame can't clobber stack data a leaf function assumed was safe.
# -mcmodel=kernel: puts kernel code in the higher-half virtual address space.
# -mno-80387 -mno-mmx -mno-sse -mno-sse2: the kernel never sets up FPU/SSE
#   state, so forbid the compiler from emitting instructions that need it.
CFLAGS := -Wall -Wextra -std=gnu11 -g \
          -ffreestanding \
          -fno-stack-protector \
          -fno-stack-check \
          -fno-pic -fno-pie \
          -m64 \
          -march=x86-64 \
          -mno-red-zone \
          -mcmodel=kernel \
          -mno-80387 -mno-mmx -mno-sse -mno-sse2 \
          -I src/kernel \
          -I include \
          -I third_party/limine

ASFLAGS := $(CFLAGS)

LDFLAGS := -m elf_x86_64 \
           -nostdlib \
           -static \
           -no-pie \
           -T linker.ld

# Gather all C and assembly files inside src/
C_FILES := $(shell find src -type f -name '*.c')
S_FILES := $(shell find src -type f -name '*.S')
OBJ_FILES := $(C_FILES:.c=.o) $(S_FILES:.S=.o)

INITRD := initrd.tar
INITRD_ROOT := build/initrd
ROOTFS_FILES := $(shell find rootfs -type f)

.PHONY: all clean iso run debug

all: $(KERNEL) $(INITRD)

$(KERNEL): $(OBJ_FILES)
	$(LD) $(LDFLAGS) $(OBJ_FILES) -o $@

# Compile individual C source files into object files
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Assemble GAS (.S) sources the same way as C, since they need the same
# freestanding/no-PIC flags and get run through the C preprocessor for the
# .macro-based ISR stub generation.
%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

# The initrd is a ustar archive of rootfs/ plus the mount-point directories
# the kernel attaches other filesystems to. Limine loads it as a module and
# the kernel mounts it at /.
$(INITRD): $(ROOTFS_FILES)
	rm -rf $(INITRD_ROOT)
	mkdir -p $(INITRD_ROOT)/dev $(INITRD_ROOT)/disk
	cp -R rootfs/. $(INITRD_ROOT)/
	tar --format=ustar -cf $@ -C $(INITRD_ROOT) $$(ls $(INITRD_ROOT))

iso: $(KERNEL) $(INITRD)
	./tools/iso.sh

run: iso
	./tools/run.sh

debug: iso
	./tools/run.sh --debug

clean:
	rm -f $(OBJ_FILES) $(KERNEL) $(INITRD)
	rm -rf iso_root atos.iso build
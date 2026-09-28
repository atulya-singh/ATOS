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

# -MMD -MP: emit a .d file per object listing the headers it included, so
# editing a header (say, a struct layout) rebuilds everything that uses it.
DEPFLAGS := -MMD -MP

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
DEP_FILES := $(OBJ_FILES:.o=.d)

# --- userspace -------------------------------------------------------------
# Same cross compiler, but hosted-style flags: no kernel code model, and
# -nostdinc so only our libc headers (plus the compiler's freestanding
# stddef/stdint/stdarg) are visible -- never the build host's glibc.
# -mgeneral-regs-only: the kernel doesn't save FPU/SSE state across
# context switches yet, so user code must not touch those registers.
# -fno-tree-loop-distribute-patterns: stop GCC from turning libc's own
# memset/memcpy loops into calls to memset/memcpy.
UCFLAGS := -std=gnu11 -O2 -Wall -Wextra \
           -ffreestanding -nostdinc \
           -isystem $(shell $(CC) -print-file-name=include) \
           -I user/libc/include -I include \
           -fno-pic -fno-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
           -ffunction-sections -fdata-sections \
           -m64 -march=x86-64 -mgeneral-regs-only

ULDFLAGS := -m elf_x86_64 -nostdlib -static -no-pie --gc-sections -T user/user.ld

LIBC_SRCS := $(wildcard user/libc/*.c) $(wildcard user/libc/*.S)
LIBC_OBJS := $(patsubst user/%,build/user/%.o,$(LIBC_SRCS))
USER_PROGS := $(notdir $(basename $(wildcard user/bin/*.c)))
USER_BINS := $(addprefix build/bin/,$(USER_PROGS))

build/user/%.c.o: user/%.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(DEPFLAGS) -c $< -o $@

build/user/%.S.o: user/%.S
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) $(DEPFLAGS) -c $< -o $@

build/bin/%: build/user/bin/%.c.o $(LIBC_OBJS) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(ULDFLAGS) $< $(LIBC_OBJS) -o $@

# --- initrd ------------------------------------------------------------------
INITRD := initrd.tar
INITRD_ROOT := build/initrd
ROOTFS_FILES := $(shell find rootfs -type f)

.PHONY: all clean iso run debug

all: $(KERNEL) $(INITRD)

$(KERNEL): $(OBJ_FILES)
	$(LD) $(LDFLAGS) $(OBJ_FILES) -o $@

# Compile individual C source files into object files
%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

# Assemble GAS (.S) sources the same way as C, since they need the same
# freestanding/no-PIC flags and get run through the C preprocessor for the
# .macro-based ISR stub generation.
%.o: %.S
	$(CC) $(ASFLAGS) $(DEPFLAGS) -c $< -o $@

# The initrd is a ustar archive of rootfs/, the userspace programs in /bin,
# and the mount-point directories the kernel attaches other filesystems to.
# Limine loads it as a module and the kernel mounts it at /.
$(INITRD): $(ROOTFS_FILES) $(USER_BINS)
	rm -rf $(INITRD_ROOT)
	mkdir -p $(INITRD_ROOT)/dev $(INITRD_ROOT)/disk $(INITRD_ROOT)/bin
	cp -R rootfs/. $(INITRD_ROOT)/
	cp $(USER_BINS) $(INITRD_ROOT)/bin/
	tar --format=ustar -cf $@ -C $(INITRD_ROOT) $$(ls $(INITRD_ROOT))

# .incbin'd data isn't seen by -MMD, so spell this dependency out.
src/kernel/dev/font.o: third_party/fonts/terminus-8x16.psf

iso: $(KERNEL) $(INITRD)
	./tools/iso.sh

run: iso
	./tools/run.sh

debug: iso
	./tools/run.sh --debug

clean:
	rm -f $(OBJ_FILES) $(DEP_FILES) $(KERNEL) $(INITRD)
	rm -rf iso_root atos.iso build

-include $(DEP_FILES) $(wildcard build/user/*/*.d)

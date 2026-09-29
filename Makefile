KERNEL := kernel.elf
.DEFAULT_GOAL := all

# Overridable so CI (native x86_64 Linux) can use plain cc/ld while
# dev.sh's Docker container (arm64 host) points these at the
# x86_64-linux-gnu cross toolchain instead.
CC ?= cc
LD ?= ld
# nm from the same binutils as $(LD) (x86_64-linux-gnu-ld -> x86_64-linux-gnu-nm).
NM ?= $(patsubst %ld,%nm,$(LD))

# -ffreestanding: don't assume standard libraries exist (no main, no printf);
#   this also implies -fno-builtin, so our libk memcpy/memset can't get
#   miscompiled into a recursive call to themselves.
# -mno-red-zone: essential once interrupts are involved, so the CPU pushing
#   a frame can't clobber stack data a leaf function assumed was safe.
# -mcmodel=kernel: puts kernel code in the higher-half virtual address space.
# -mno-80387 -mno-mmx -mno-sse -mno-sse2: the kernel never sets up FPU/SSE
#   state, so forbid the compiler from emitting instructions that need it.
# -fno-omit-frame-pointer: panic backtraces walk the saved-rbp chain.
CFLAGS := -Wall -Wextra -std=gnu11 -g \
          -ffreestanding \
          -fno-omit-frame-pointer \
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
# -fno-tree-loop-distribute-patterns: stop GCC from turning libc's own
# memset/memcpy loops into calls to memset/memcpy.
UCFLAGS := -std=gnu11 -O2 -Wall -Wextra \
           -ffreestanding -nostdinc \
           -isystem $(shell $(CC) -print-file-name=include) \
           -I user/libc/include -I include \
           -fno-pic -fno-pie -fno-stack-protector \
           -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
           -ffunction-sections -fdata-sections \
           -m64 -march=x86-64

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

# --- ports -------------------------------------------------------------------
# Third-party-style programs under ports/<name>/, each with a PORTBUILD
# recipe; tools/port.sh builds one into build/ports/<name>/root/, which is
# merged into the initrd (programs land in /usr/bin). See docs/ports.md.
PORTS := $(notdir $(patsubst %/PORTBUILD,%,$(wildcard ports/*/PORTBUILD)))
PORT_STAMPS := $(foreach p,$(PORTS),build/ports/$(p)/.built)

# --- initrd ------------------------------------------------------------------
INITRD := initrd.tar
INITRD_ROOT := build/initrd
ROOTFS_FILES := $(shell find rootfs -type f)

# --- host unit tests ---------------------------------------------------------
# Kernel code with no hardware dependencies, compiled natively (not
# freestanding) with ASan + UBSan and run as an ordinary program.
HOST_CC ?= cc
HOST_TEST_KERNEL_SRCS := src/kernel/mm/heap_core.c src/kernel/lib/path.c \
                         src/kernel/fs/fat_names.c src/kernel/fs/tar.c src/kernel/lib/kprintf.c \
                         src/kernel/net/inet.c ports/grep/regex.c
HOST_TEST_SRCS := $(wildcard tests/host/*.c)
HOST_TEST_BIN := build/host/run-tests

$(HOST_TEST_BIN): $(HOST_TEST_SRCS) $(HOST_TEST_KERNEL_SRCS) $(wildcard tests/host/*.h)
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -g -O1 -Wall -Wextra -DATOS_HOST_TEST -pthread \
		-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer \
		-I src/kernel -I include -I ports $(HOST_TEST_SRCS) $(HOST_TEST_KERNEL_SRCS) -o $@

test-host: $(HOST_TEST_BIN)
	./$(HOST_TEST_BIN)

.PHONY: all clean iso run debug test-host ports

all: $(KERNEL) $(INITRD)

# Two-stage link for the panic symbol table (see tools/gensyms.sh): link
# with an empty table, generate the real one from that, link again, and
# check no function moved in between.
KSYMS_DIR := build/kernel

$(KSYMS_DIR)/ksyms-empty.S: tools/gensyms.sh
	@mkdir -p $(dir $@)
	./tools/gensyms.sh $(NM) - > $@

$(KSYMS_DIR)/kernel-stage1.elf: $(OBJ_FILES) $(KSYMS_DIR)/ksyms-empty.o linker.ld
	$(LD) $(LDFLAGS) $(OBJ_FILES) $(KSYMS_DIR)/ksyms-empty.o -o $@

$(KSYMS_DIR)/ksyms.S: $(KSYMS_DIR)/kernel-stage1.elf tools/gensyms.sh
	./tools/gensyms.sh $(NM) $< > $@

$(KSYMS_DIR)/%.o: $(KSYMS_DIR)/%.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(KERNEL): $(OBJ_FILES) $(KSYMS_DIR)/ksyms.o linker.ld
	$(LD) $(LDFLAGS) $(OBJ_FILES) $(KSYMS_DIR)/ksyms.o -o $@
	@./tools/gensyms.sh $(NM) $@ | cmp -s - $(KSYMS_DIR)/ksyms.S || \
		{ echo "error: function addresses moved between link stages" >&2; rm -f $@; exit 1; }

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
$(INITRD): $(ROOTFS_FILES) $(USER_BINS) $(PORT_STAMPS)
	rm -rf $(INITRD_ROOT)
	mkdir -p $(INITRD_ROOT)/dev $(INITRD_ROOT)/disk $(INITRD_ROOT)/bin \
		$(INITRD_ROOT)/usr/bin $(INITRD_ROOT)/usr/share/ports
	cp -R rootfs/. $(INITRD_ROOT)/
	cp $(USER_BINS) $(INITRD_ROOT)/bin/
	for p in $(PORTS); do cp -R build/ports/$$p/root/. $(INITRD_ROOT)/; done
	cat /dev/null $(foreach p,$(PORTS),build/ports/$(p)/index-line) \
		> $(INITRD_ROOT)/usr/share/ports/INDEX
	tar --format=ustar -cf $@ -C $(INITRD_ROOT) $$(ls $(INITRD_ROOT))

ports: $(PORT_STAMPS)

# Rebuilt when anything in the port's directory changes (hence the second
# expansion, which lets the prerequisite list use the stem).
.SECONDEXPANSION:
build/ports/%/.built: $$(wildcard ports/$$*/*) $(LIBC_OBJS) user/user.ld tools/port.sh
	CC="$(CC)" LD="$(LD)" UCFLAGS="$(UCFLAGS)" ULDFLAGS="$(ULDFLAGS)" LIBC_OBJS="$(LIBC_OBJS)" \
		./tools/port.sh ports/$* build/ports/$*

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

# Architecture

ATOS is a 64-bit x86 kernel written in freestanding C (plus a little
GNU assembly). The Limine bootloader loads it. It runs a small Unix-like
userspace on up to 16 CPUs.

## Boot flow

1. **Limine** loads `kernel.elf` into the higher half, loads `initrd.tar`
   as a module, starts the bootstrap processor (BSP) in long mode on
   Limine's page tables, and calls `kmain` (`src/kernel/main.c`). The
   requests the kernel makes (memory map, HHDM, framebuffer, modules,
   RSDP, SMP, kernel file/cmdline) live in the `.limine_requests` section.
2. **`kmain`** points GS at CPU 0's `struct cpu` so spinlocks work. It
   brings up serial and the framebuffer console, loads the GDT/TSS and IDT
   (the legacy PIC is remapped and fully masked), and initializes the
   physical allocator.
3. **`vmm_init`** builds ATOS's own page tables (HHDM plus the kernel
   image with W^X per section) and switches CR3 and the stack in one step.
   It continues in `kmain_stage2`, which becomes CPU 0's idle task.
4. **`kmain_stage2`** brings up the rest in dependency order:
   - heap
   - ACPI (MADT/FADT)
   - LAPIC + IOAPIC
   - scheduler
   - initrd and devfs
   - keyboard
   - PCI, then virtio-blk and virtio-net
   - FAT mount
   - network configuration
   - LAPIC timer calibration
   - SMP bring-up

   It then spawns the kernel self-tests. Once they finish, it starts
   `/bin/init`, which runs the shell.

## Kernel command line

Set with `cmdline:` in `limine.conf`:

| Option | Effect |
|--------|--------|
| `init=<path>` | Run `<path>` as the first process instead of `/bin/init` |
| `selftest-exit` | Headless CI mode: exit QEMU (isa-debug-exit) with the self-test verdict instead of starting userspace |
| `crashtest=pagefault\|stackoverflow\|assert` | Deliberately crash, to test the panic path |
| `ip=`, `netmask=`, `gw=`, `dns=` | Static network configuration (defaults match QEMU user networking) |

## Source layout

```
src/kernel/
  main.c            boot sequence
  selftest.c        boot-time self-tests;  crashtest.c: deliberate crashes
  arch/x86_64/      GDT/TSS, IDT + ISR stubs, LAPIC, IOAPIC, per-CPU data, SMP, MSRs
  acpi/             RSDP/RSDT/XSDT, MADT, FADT, \_S5, poweroff/reboot
  mm/               boot memory map, PMM, VMM, heap (heap_core.c is hardware-free)
  sched/            tasks, context switch (switch.S), wait queues, mutexes
  proc/             ELF loader, process spawn/exec/brk
  sys/              syscall dispatch, user-pointer checks
  fs/               VFS, initrd (tar), devfs, FAT32 (+ hardware-free name/tar helpers)
  dev/              PCI, virtio-blk, virtio-net, keyboard, serial, fbcon, console, RTC, timer
  net/              Ethernet/ARP, IPv4/ICMP/UDP, TCP, sockets (inet.c is hardware-free)
  lib/              kprintf, string, spinlock, SPSC ring, panic + ksyms, cmdline, path
include/atos/abi.h  the kernel/userspace ABI, shared by both sides
user/libc/          the C library;  user/bin/: base programs (installed in /bin)
ports/              ported programs (installed in /usr/bin); see ports.md
rootfs/             files copied verbatim into the initrd
tests/host/         host-side unit tests
tools/              build, run, test, and packaging scripts
```

## Virtual address map

| Range | Use |
|-------|-----|
| `0x0000000000400000` | user program image (ELF), then the brk heap |
| `0x00007FFFFFFFF000` (top, downward) | user stack (16 KiB) |
| HHDM (Limine's offset) | all physical RAM, plus device registers mapped on demand |
| `0xFFFFA00000000000` | kernel heap (4 MiB) |
| `0xFFFFB00000000000` | kernel stacks: 32 KiB slots, 16 KiB stack + unmapped guard below |
| `0xFFFFFFFF80000000` | kernel image (`-mcmodel=kernel`) |

## Concurrency model

- **Preemption and CPUs.** The kernel is preemptible and runs on every
  CPU. A 100 Hz per-CPU LAPIC timer drives preemption.
- **Short critical sections** use spinlocks. Any lock that can be taken
  from an interrupt handler is taken with interrupts disabled.
- **Long or blocking sections** (disk I/O, the network stack) use
  sleeping mutexes.
- **Blocking waits** use the race-free `wait_event` pattern on wait
  queues.

See [smp.md](smp.md) and [scheduler.md](scheduler.md).

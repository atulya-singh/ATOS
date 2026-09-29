# ATOS documentation

One page per subsystem. Each describes the design, the key data
structures, where the code lives, and the known limits. The source
comments carry the finer detail.

| Page | Covers |
|------|--------|
| [architecture.md](architecture.md) | Boot flow, source layout, address-space map, how the pieces fit |
| [memory.md](memory.md) | Physical allocator, page tables, kernel heap, user address spaces |
| [smp.md](smp.md) | GDT/IDT, LAPIC/IOAPIC, per-CPU data, AP bring-up, spinlocks, TLB shootdown |
| [scheduler.md](scheduler.md) | Tasks, preemption, wait queues, mutexes, the reaper, processes |
| [syscalls.md](syscalls.md) | The kernel/user ABI and every system call |
| [filesystems.md](filesystems.md) | VFS, initrd (ustar), devfs, FAT32 |
| [drivers.md](drivers.md) | PCI, virtio-blk, virtio-net, ACPI, keyboard, consoles, RTC, timer |
| [net.md](net.md) | The IPv4 stack: ARP, ICMP, UDP, TCP, sockets, and the network tools |
| [userspace.md](userspace.md) | libc, the shell, and the programs in /bin |
| [ports.md](ports.md) | The ports system: writing a PORTBUILD, building into /usr/bin, `pkg` |
| [testing.md](testing.md) | Host unit tests, boot self-tests, the QEMU smoke test, CI |
| [debugging.md](debugging.md) | Panic screen, backtraces, crash tests, gdb |

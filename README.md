# ATOS

A 64-bit x86 operating system written from scratch in C. It boots with
Limine and runs on up to 16 CPUs.

- **Kernel**
  - Preemptive SMP scheduler, per-CPU data, spinlocks, TLB shootdown
  - W^X page tables, per-process address spaces
  - ELF processes with `fork`/`exec`/`waitpid`
  - Panic screen with a symbolized backtrace
- **Storage:** VFS with a tar initrd, devfs, and a read/write FAT32
  driver (long file names, mkdir, rename, delete) over virtio-blk
- **Networking:** virtio-net and an IPv4 stack (ARP, DHCP, ICMP, UDP, TCP)
  with BSD sockets, plus `ping`, `nslookup`, `wget`, and an `httpd`
- **Hardware:** ACPI power-off and reboot, LAPIC/IOAPIC, PCI
- **Userspace:** a small libc, a shell, core utilities, and a ports
  system (`hexdump`, `grep`, `calc`, `fortune`) managed with `pkg`
- **Tests:** host unit tests (ASan/UBSan), boot-time self-tests, and a
  QEMU smoke test with 100+ checks, all run in CI

## Quick start

You need Docker (`dev.sh` provides the cross toolchain, xorriso, and
QEMU), or on x86-64 Linux: gcc, binutils, xorriso, mtools, and
qemu-system-x86.

```
./dev.sh make run        # build and boot; the shell is on the serial console
./dev.sh make test-host  # host unit tests
./dev.sh sh -c 'make iso && tools/smoke-test.sh'   # the full boot test
```

In the guest, try `help`, `ls /bin`, `pkg list`, `ping 10.0.2.2`, or
`httpd`. For `httpd`, fetch from the host with
`curl localhost:8080/README`.

## Documentation

See [docs/](docs/README.md): architecture, memory, SMP, scheduler,
syscalls, filesystems, drivers, networking, userspace, ports, testing,
and debugging.

# Testing

There are three layers, all run by CI (`.github/workflows/ci.yml`) on
every push.

## 1. Host unit tests (`make test-host`)

`tests/host/` compiles hardware-free kernel modules natively, with
AddressSanitizer and UndefinedBehaviorSanitizer (`-DATOS_HOST_TEST`), and
runs them as an ordinary program:

| Test file | Covers |
|-----------|--------|
| `test_heap.c` | The first-fit allocator: splitting, coalescing, a structural check after random churn |
| `test_path.c` | Path normalization |
| `test_fat_names.c` | 8.3 encoding, LFN checksums, `~N` aliases, name validity, FAT dates |
| `test_tar.c` | ustar octal fields and checksums, against a real host `tar` header |
| `test_kprintf.c` | Every `kprintf` conversion, including the `LONG_MIN` and trailing-`%` edge cases |
| `test_inet.c` | The Internet checksum (RFC 1071 example, a real IPv4 header), pseudo-headers, IP parsing |
| `test_regex.c` | The grep port's regex engine |
| `test_ring.c` | The SPSC ring: FIFO order and fullness, plus a two-thread stress run of 2 million bytes |

The harness (`test.h`) is tiny. `TEST(name)` registers a test, and
`CHECK*` records failures without stopping the test. A new test file in
`tests/host/` is picked up automatically. If it needs kernel code, add
that source to `HOST_TEST_KERNEL_SRCS`.

## 2. Boot-time self-tests (`selftest.c`)

The kernel checks itself before starting userspace. Results are printed
as `ATOS: self-test summary: N passed, M failed`:

| Area | What it checks |
|------|----------------|
| Heap and PMM | Heap behavior and leak accounting |
| Scheduler | Preemption |
| User mode | A ring-3 hello, a kernel pointer rejected with `-EFAULT`, a faulting process killed cleanly |
| VFS | `/dev/null`, `/dev/console`, and `readdir` semantics |
| initrd | Reads from the initrd |
| Disk | A pattern write and readback on a scratch disk (when one is attached) |
| SMP | 8 workers doing 160,000 spinlocked and 3,200 mutexed increments across CPUs |

The tests' tasks must all be reaped, and the heap and PMM free counts
must return to their baselines.

With `selftest-exit` on the command line, the kernel leaves QEMU through
the isa-debug-exit device with the verdict as the exit status, instead of
starting userspace.

## 3. The QEMU smoke test (`tools/smoke-test.sh`)

This boots the real image the way a user would, in 4-CPU q35 QEMU. It
attaches two virtio disks (a scratch disk and a FAT32 volume) and a
virtio-net NIC on user-mode networking. Then it:

1. Waits for the shell.
2. Types commands through QEMU's monitor (`sendkey`), covering files,
   redirection, FAT writes, ports, and networking against
   `tools/net-test-server.pl`.
3. Finishes with `poweroff` and requires QEMU to exit.

Afterwards it greps the serial log for every expected line. It also
checks from the host side:

- the scratch disk's pattern
- FAT files through `mtools`
- `fsck.fat`
- the TCP download, byte for byte
- a file fetched from the guest's `httpd`

**Variant boots** reuse the image with a different kernel command line:

- the three crash tests
- `selftest-exit`
- `init=/bin/reboot`
- DHCP fallback: a NIC on an empty QEMU hub, so nobody answers and the
  static configuration must kick in
- `ip=` on the command line: a static address with no DHCP traffic

Run it locally with `./dev.sh tools/smoke-test.sh` after `make iso`. It
prints one `PASS`/`FAIL` line per check and exits non-zero on any
failure, dumping the serial log.

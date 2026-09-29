# System calls

The ABI lives in one header, `include/atos/abi.h`. The kernel and the
userspace libc both include it, so they cannot drift apart. Dispatch is
in `src/kernel/sys/syscall.c`.

## Convention

- Enter with `int 0x80`.
- The syscall number goes in `rax`. Arguments go in `rdi`, `rsi`, `rdx`,
  `r10`, and `r8`, the same order as Linux's `syscall`.
- The result comes back in `rax`. Failures return `-errno`. The libc
  wrappers turn that into `-1` plus `errno`.
- Every register except `rax` is preserved.

The kernel checks each user pointer with `user_range_ok` before touching
it. That check walks the caller's page tables to confirm the range is
mapped and user-accessible. A single `read`/`write` moves at most 64 KiB;
callers loop, as with any short I/O.

## Table

| # | Name | Arguments | Returns |
|---|------|-----------|---------|
| 0 | write | fd, buf, len | bytes written |
| 1 | exit | code | — |
| 2 | yield | — | 0 |
| 3 | read | fd, buf, len | bytes read, 0 at EOF |
| 4 | open | path, flags (`O_RDONLY/WRONLY/RDWR`, `O_CREAT`, `O_TRUNC`, `O_APPEND`) | fd |
| 5 | close | fd | 0 |
| 6 | seek | fd, offset, whence | new offset |
| 7 | readdir | fd, index, `struct atos_dirent *` | 0, or `-ENOENT` past the end |
| 8 | fstat | fd, `struct atos_stat *` | 0 |
| 9 | brk | new break (0 queries) | the break |
| 10 | fork | — | child pid / 0 |
| 11 | exec | path, argv | no return on success |
| 12 | waitpid | pid or -1, `int *status` | pid |
| 13 | getpid | — | pid |
| 14 | dup2 | oldfd, newfd | newfd |
| 15 | reboot | `ATOS_REBOOT_POWEROFF` / `_RESTART` | no return on success |
| 16 | socket | `ATOS_SOCK_STREAM` / `_DGRAM` / `_ICMP` | fd |
| 17 | bind | fd, `struct atos_sockaddr_in *` | 0 |
| 18 | connect | fd, addr | 0 |
| 19 | listen | fd, backlog | 0 |
| 20 | accept | fd, peer addr or NULL | new fd |
| 21 | sendto | fd, buf, len, dest or NULL | bytes sent |
| 22 | recvfrom | fd, buf, len, src or NULL | bytes, 0 at EOF |
| 23 | sockopt | fd, `ATOS_SO_RCVTIMEO`, milliseconds | 0 |
| 24 | netinfo | `struct atos_netinfo *` | 0, or `-ENODEV` |
| 25 | uptime | — | milliseconds since boot |
| 26 | sleep | milliseconds | 0 |

A socket is a file descriptor, so `read`, `write`, `close`, `dup2`, and
inheritance across `fork` all work on it. See [net.md](net.md) for socket
semantics.

## Adding a syscall

1. Add the number (and any structs or constants) to `abi.h`.
2. Add a `case` in `syscall_handler`, with a `sys_*` function that vets
   its user pointers.
3. Add a wrapper in `user/libc/syscall.c` and a prototype in the matching
   libc header.

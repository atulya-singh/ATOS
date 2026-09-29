# Userspace

## libc (`user/libc/`)

A small C library, statically linked into every program:

| File | Provides |
|------|----------|
| `crt0.S` | `_start`: gets argc/argv from the stack, calls `main`, then `exit` |
| `syscall.c` | POSIX-style wrappers (`read`, `write`, `open`, `fork`, `execv`, `waitpid`, `dup2`, `sbrk`, `sleep`, sockets, ...). They return -1 and set `errno` |
| `stdio.c` | `printf`, `dprintf`, `snprintf`, `vsnprintf`, `putchar`, `puts`, over fds (no `FILE *` buffering yet) |
| `stdlib.c` | `malloc`/`calloc`/`realloc`/`free` over `brk`, `atoi`, `exit` |
| `string.c` | The `mem*`/`str*` basics, `strstr`, `strerror` |
| `net.c` | `inet_aton`/`inet_ntoa`/`inet_addr`, a DNS resolver, `gethostbyname` |

Headers live in `user/libc/include/`. They mirror the POSIX names
(`unistd.h`, `fcntl.h`, `sys/socket.h`, `netinet/in.h`, `arpa/inet.h`,
`netdb.h`). `atos.h` holds ATOS-only calls such as `readdir`, `fstat`,
`netinfo`, `uptime_ms`, `msleep`, and `dns_resolve`.

User code is built with `-mgeneral-regs-only`, because the kernel does
not yet save FPU/SSE state across context switches. That means no
floating point.

## Programs

**Base system** (`user/bin/`, installed in `/bin`):

| Program | Does |
|---------|------|
| `init` | Shows `/etc/motd`, runs `/bin/sh`, and restarts it when it exits |
| `sh` | The shell: runs commands (searching `/bin`, then `/usr/bin`), handles `>` and `>>` redirection and quoted words, prints `[exit N]` for failures. Built-ins: `help`, `exit` |
| `ls`, `cat`, `echo`, `wc` | The classics |
| `libctest` | libc and kernel conformance checks, run by the smoke test |
| `poweroff`, `reboot` | Through ACPI |
| `ifconfig`, `ping`, `nslookup`, `wget`, `httpd` | Networking (see [net.md](net.md)) |
| `pkg` | Lists and describes installed ports (see [ports.md](ports.md)) |

**Ports** (`ports/`, installed in `/usr/bin`): `hexdump`, `grep`,
`calc`, `fortune`.

## Adding a base program

Drop `user/bin/<name>.c` (with a `main`) into the tree. The Makefile
picks it up, links it against libc, and installs it as `/bin/<name>`.
Anything self-contained that isn't core to the system is better written
as a port.

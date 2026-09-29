# Ports

The ports system builds optional programs, and the data they need, into
the system image. They are installed under `/usr`, separate from the base
system in `/bin`. It follows the spirit of BSD ports and Arch PKGBUILDs:

- Each port is a directory holding a recipe and its sources.
- One script builds any port.
- The build records what each port installed.

## Layout

```
ports/<name>/
  PORTBUILD       recipe (shell variables)
  *.c, *.h        sources
  other files     data to install
```

## PORTBUILD

```sh
PORT_VERSION=1.0                                      # required
PORT_DESCRIPTION="search files for lines matching a regular expression"  # required
PORT_LICENSE=MIT
PORT_PROGRAMS="grep"                                  # built into /usr/bin
grep_SRCS="grep.c regex.c"                            # default: <program>.c
PORT_FILES="fortunes:/usr/share/fortune/fortunes"     # <file>:<absolute install path>
PORT_CFLAGS="-DSOMETHING"                             # extra compiler flags
```

A port may build several programs, each with its own `<prog>_SRCS`.
Sources are compiled with the userspace flags and the libc headers, with
the port directory on the include path. They are linked statically
against libc.

## Building

`make` builds every port as part of the initrd. `make ports` builds only
the ports. For each port, `tools/port.sh`:

1. Builds into `build/ports/<name>/root/` (a staging root).
2. Writes the metadata file `root/usr/share/ports/<name>`, which lists
   the name, version, description, license, and every installed file.
3. Writes a line for `/usr/share/ports/INDEX`.

The Makefile then merges every staging root into the initrd. A port is
rebuilt when any file in its directory changes, or when libc does.

## On the running system

```
atos$ pkg list
calc       1.0    64-bit integer expression calculator
fortune    1.0    print a random adage about systems programming
grep       1.0    search files for lines matching a regular expression
hexdump    1.0    canonical hex+ASCII dump of files (like hexdump -C)
4 port(s) installed
atos$ pkg info grep
atos$ grep -n ^1[0-2]3$ /disk/numbers.txt
```

The shell searches `/bin`, then `/usr/bin`.

## Included ports

| Port | What it is |
|------|------------|
| `hexdump` | `hexdump [-n len] [-s skip] [file]` in the `-C` layout, collapsing repeated lines to `*` |
| `grep` | `grep [-invc] pattern [file...]`, backed by its own small backtracking regex engine (`. [] [^] * + ? ^ $ \d \w \s`), which is host-tested |
| `calc` | 64-bit integer expressions with C operators and precedence; decimal, `0x`, and `0b` literals |
| `fortune` | A random adage from `/usr/share/fortune/fortunes`; demonstrates `PORT_FILES` |

## Porting a program

1. `mkdir ports/<name>`, then add the sources and a `PORTBUILD`.
2. Stick to what libc offers (see [userspace.md](userspace.md)). Missing
   pieces are best added to libc, where every program benefits.
3. Run `make`, then boot and run it. Add a line to `tools/smoke-test.sh`
   to keep it working.

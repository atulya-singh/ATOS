# Debugging

## Serial log

Everything the kernel prints (`kprintf`) goes to COM1 as well as the
screen. `make run` puts serial on your terminal. Serial output is the
fast debug loop.

## Kernel panics

`panic(fmt, ...)`, `KASSERT(cond)`, and any exception in kernel mode all
end on the panic screen: white text on red, mirrored to serial. It shows:

- the message, or the exception name with its error code and CR2
- a likely cause, when one can be inferred. For example, `rsp` inside a
  guard page means "kernel stack overflow".
- the CPU (`cpu: X of N`) and task (`task: id (name)`)
- all registers
- a **symbolized backtrace** from walking the `rbp` chain. The kernel is
  built with `-fno-omit-frame-pointer`, and repeated frames collapse
  into "... same frame N more time(s)".

The panic stops the other CPUs with an NMI and ends with
`--- system halted ---`.

**Symbols.** The kernel is linked twice. `tools/gensyms.sh` extracts the
function symbols from the first link into a table, which the second link
embeds (`lib/ksyms.c`). The Makefile checks that no function moved
between the two links.

A fault in user mode never panics. It kills the process, which is
reported as, for example, `killed: Page Fault`.

## Crash tests

To see the panic path, boot with `crashtest=pagefault`,
`crashtest=stackoverflow`, or `crashtest=assert` on the kernel command
line. The smoke test boots all three and checks the output.

## gdb

`make debug` starts QEMU paused, with a gdb stub on `localhost:1234`.
Then:

```
gdb -x .gdbinit
```

`.gdbinit` loads `kernel.elf`, connects, and stops at `kmain`. `break
panic` is a useful next breakpoint.

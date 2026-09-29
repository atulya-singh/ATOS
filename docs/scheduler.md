# Scheduler, tasks, and processes

Code: `src/kernel/sched/`, `src/kernel/proc/`.

## Tasks

A `struct task` is either a kernel thread or a user process.

- **Kernel state:** a kernel stack, a saved `rsp`, and a CR3.
- **Process state:** fds, brk bounds, parent/child links, and the exit
  code.

Task states:

| State | Meaning |
|-------|---------|
| `READY` | Runnable, including while running |
| `SLEEPING` | Waiting until `wake_tick` |
| `BLOCKED` | On a wait queue |
| `ZOMBIE` | Exited, resources not yet freed |
| `DEAD` | Freed. Only the struct remains, for the parent's `waitpid` |

There are at most 64 tasks.

## Scheduling

- **Run list.** Round-robin over one circular run list shared by all
  CPUs, starting from a global cursor.
- **Slices.** Time slices are 5 ticks (50 ms) of the 100 Hz timer.
- **Idle.** Each CPU has an idle task that is not on the run list. It
  runs only when nothing else is runnable.
- **Pinning.** A task marked `on_cpu` is running somewhere, or a CPU is
  still standing on its stack mid-switch. No other CPU may pick it up.

### The switch protocol

One `sched_lock`, taken with interrupts off, guards all scheduler state.
It is held across `context_switch` (`switch.S`):

1. The outgoing task takes the lock and switches.
2. The incoming task runs `sched_finish_switch`, which clears the
   outgoing task's `on_cpu` and releases the lock.

New tasks do this in `task_trampoline` and `fork_return`. This hand-off
is what makes it safe for another CPU to pick up, or the reaper to free,
the previous task.

## Blocking: wait queues and mutexes

Blocking uses the race-free `wait_event(wq, cond)` macro. It registers
the task on the queue (`wait_prepare`) before the final check of `cond`,
so a wakeup that lands in between cannot be lost. `wait_queue_wake_all`
is safe from interrupt context.

`struct mutex` is a sleeping lock (an atomic flag plus a wait queue) for
long sections that may block, such as disk I/O and the network stack.

## Exit and reaping

`task_exit`:

1. Closes the task's fds.
2. Orphans its children.
3. Wakes its parent.
4. Becomes a zombie.

The **reaper** kernel thread later frees the zombie's address space and
kernel stack. It waits until the task is no longer `on_cpu`, and it does
the TLB shootdown the stack needs. If a parent may still call `waitpid`,
the struct is kept as `DEAD` until it does.

## Processes (`proc/`)

- **`process_spawn(path, argc, argv)`** loads an ELF64 executable
  (`elf.c`) into a fresh address space at its linked addresses (from
  `0x400000`), builds the argv block on a 16 KiB user stack, and starts
  it with fds 0–2 on `/dev/console`.
- **`fork`** deep-copies the address space, fds, and brk. The child
  resumes from the same syscall frame with `rax = 0`.
- **`exec`** first copies the path and argv (up to 32 arguments and
  3 KiB) out of the old address space, then replaces the address space.
- **`brk`** grows or shrinks the heap after the program image, with
  Linux's `brk` contract.
- A user fault kills only that process. The kernel logs it as
  `killed: Page Fault`.

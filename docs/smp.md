# CPUs, interrupts, and SMP

Code: `src/kernel/arch/x86_64/`, `src/kernel/lib/spinlock.h`, `src/kernel/lib/ring.h`.

## Per-CPU state (`percpu.c/h`)

Each CPU has a `struct cpu`. It holds:

- a self pointer (at offset 0) and the current task (at offset 8)
- its index and LAPIC id
- its idle task
- its own GDT and TSS
- its first kernel stack

The GS base points at the CPU's own struct, so `this_cpu()` and
`sched_current()` are each a single `mov %gs:N`. User mode has its own GS
base. `swapgs` runs on every ring-3 boundary:

- in the ISR stubs, based on the saved CS
- in `fork_return`
- in `jump_to_user`

A reload of `%gs` resets the base, so `percpu_set` must follow any GDT
load.

## Descriptor tables (`gdt.c`, `idt.c`, `isr_stubs.S`, `isr.c`)

- **GDT.** Each CPU has its own GDT with kernel and user code/data
  segments plus its own TSS. The TSS provides `rsp0` (the current task's
  kernel stack, updated on every switch) and an IST stack for double
  faults.
- **IDT.** The IDT is shared:
  - vectors 0–31: exceptions
  - vector 32: the LAPIC timer
  - vectors 33–47: ISA IRQs routed through the IOAPIC
  - `0x80`: syscalls (DPL 3)
  - `0xF0`: TLB shootdown
  - `0xFF`: spurious
- **Legacy PIC.** The 8259s are remapped away from the exception
  vectors and fully masked.

## APICs (`lapic.c`, `ioapic.c`)

- **LAPIC.** The LAPIC's MMIO base comes from the MADT and is mapped
  uncached. Each CPU enables its own LAPIC.
- **Timer.** The LAPIC timer is calibrated once against PIT channel 2 to
  fire at 100 Hz (`TIMER_HZ`). Every CPU runs it on vector 32. The BSP's
  tick also advances the global `timer_ticks()`.
- **IOAPIC.** The IOAPIC routes ISA IRQs to the BSP, honoring the MADT's
  interrupt source overrides. Only the keyboard (IRQ 1) currently uses
  one; serial output is polled, and PCI devices are polled by their
  drivers.
- **IPIs.** Targeted IPIs carry TLB shootdowns. A broadcast NMI halts
  the other CPUs during a panic.

## Bringing up the other CPUs (`smp.c`, `smp_asm.S`)

The APs come from Limine's SMP response. For each AP, the BSP:

1. Creates an idle task and records that task's stack top in
   `cpus[i].boot_stack_top`. This lives in `.bss`, because at that point
   the AP runs on Limine's page tables, where the heap isn't mapped.
2. Writes the AP's `goto_address`.
3. Waits for the AP to report itself online.

The AP then:

1. Sets EFER.NXE and CR0.WP.
2. Switches to the kernel's CR3 and its idle stack (`smp_enter_kernel`).
3. Loads its GDT/TSS and the IDT.
4. Enables its LAPIC and starts its timer.
5. Idles. From its first tick, the scheduler hands it work.

## Spinlocks (`lib/spinlock.h`)

A spinlock is a test-and-test-and-set lock with `pause`. It records its
owner CPU, so taking a lock the CPU already holds panics ("spinlock
recursion") instead of deadlocking. The `_irqsave` variants disable
interrupts first; use them for any lock an interrupt handler also takes.

The kernel's spinlocks:

- `sched_lock`
- `console_lock` (serial + framebuffer output)
- `pmm_lock`
- `heap_lock`
- PCI config
- the CMOS lock
- the keyboard reader lock

## Lock-free ring (`lib/ring.h`)

A single-producer/single-consumer byte ring built on acquire/release
atomics. The keyboard IRQ produces and readers consume. A host test
stress-tests it with two threads.

## TLB shootdown

When the kernel unmaps memory that other CPUs may have cached (a freed
kernel stack), `tlb_shootdown(start, len)`:

1. Sends vector `0xF0` to every other online CPU.
2. Waits until each has run `invlpg` over the range.

It must run with interrupts enabled and no spinlock held, so that two
CPUs shooting down at once cannot deadlock. This is why a dedicated
reaper thread frees exited tasks, rather than `schedule()` doing it.

## Panics on SMP

`panic` sends an NMI to the other CPUs. Their NMI handler sees a panic in
progress and halts. The panic then force-releases the console lock, which
another CPU may have held when it stopped, before printing.

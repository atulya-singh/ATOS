# Memory management

Code: `src/kernel/mm/`.

## Physical memory (`pmm.c`)

The physical allocator is a bitmap with one bit per 4 KiB page, up to
the highest address in Limine's memory map. The bitmap itself sits in
the first usable region large enough to hold it. Only `USABLE` memory
map entries are marked free, and page 0 is never handed out, so a
return value of 0 always means "out of memory".

- `pmm_alloc_page()` returns one zeroed page.
- `pmm_alloc_contiguous(n)` returns a physically contiguous run of
  zeroed pages, for device DMA rings and buffers.
- `pmm_free_page()` releases one page.

A spinlock (`pmm_lock`) makes the allocator safe from any CPU.

## Page tables (`vmm.c`, `vmm_asm.S`)

`vmm_init` replaces Limine's page tables with ATOS's own:

- **The HHDM** (higher-half direct map): every memory-map range at
  Limine's HHDM offset, so `phys_to_virt(p)` is just an addition.
- **The kernel image**, mapped section by section with W^X:
  - `.text`: read + execute
  - `.rodata`: read-only + NX
  - `.data`/`.bss`: read-write + NX

  CR0.WP is set, so the kernel itself cannot write to read-only pages.

Switching CR3 and the stack happens in one assembly step
(`vmm_switch_and_continue`), because Limine's boot stack may not be
mapped in the new tables.

- `vmm_map_phys(phys, len, flags)` maps things outside the memory map,
  such as LAPIC/IOAPIC registers (with `VMM_NOCACHE`) or ACPI tables,
  into the HHDM on demand.
- `vmm_prealloc_tables` builds the intermediate tables for a region
  up front, so later map/unmap cycles there (kernel stacks) never
  allocate.

### User address spaces

Each process has its own PML4. The upper (kernel) half is shared by
copying the kernel's top-level entries, so kernel mappings are the same
in every address space. The main operations:

- `vmm_create_address_space`: a new PML4 sharing the kernel half.
- `vmm_map_user` / `vmm_unmap_user`: map or remove a user page.
- `vmm_clone_address_space`: a deep copy of the lower half, for `fork`.
  There is no copy-on-write yet.
- `vmm_destroy_address_space`: frees every user page and table.
- `vmm_user_range_ok`: how syscalls vet user pointers. It checks that
  every page is present and user-accessible, and writable if the kernel
  will write to it.

## Kernel heap (`heap.c`, `heap_core.c`)

The heap lives in a 128 MiB window at `0xFFFFA00000000000`. It starts
with 1 MiB mapped and grows on demand: when `kmalloc` finds no block
that fits, it maps more PMM pages at the heap's end (at least 256 KiB at
a time) and extends the arena (`heap_arena_grow`). The page tables for
the whole window are allocated up front (`vmm_prealloc_tables`, 256 KiB),
so growing only writes leaf entries. That keeps it safe under the heap
spinlock with no lock on the kernel's tables, and it means the pages a
growth takes are exactly the heap pages. The heap never shrinks: freed
memory stays in the arena.

The boot self-tests account for growth. `heap_grown_bytes` counts it,
and a leak check that spans a growth expects the PMM to have lost
exactly those pages and the free heap to have gained exactly those
bytes. The heap self-test forces a growth on every boot.

The allocator (`heap_core.c`) is first-fit over
address-ordered blocks that coalesce on free. It has no knowledge of
paging or locking, so the host tests run it natively under ASan.
`heap.c` wraps it with a spinlock to provide `kmalloc`/`kfree`.
`heap_arena_check` verifies the structure: links agree, blocks are
contiguous, and no two free blocks are adjacent.

## Kernel stacks

Each task gets a 32 KiB slot at `0xFFFFB00000000000 + slot * 32 KiB`.
Only the top 16 KiB is mapped. The unmapped bottom half is a guard, so
a stack overflow page-faults immediately. The fault then escalates to a
double fault, which runs on its own IST stack, and the panic handler
reports it as a stack overflow. Freeing a stack that has been used first
shoots down other CPUs' TLB entries for it (see [smp.md](smp.md)).

## Limits

- No demand paging, copy-on-write, swapping, or `mmap`.
- The kernel heap is a fixed 4 MiB.
- User programs get a fixed 16 KiB stack. Their heap grows through `brk`.

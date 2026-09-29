#include "process.h"
#include "elf.h"
#include "../arch/x86_64/idt.h"
#include "../lib/kprintf.h"
#include <stddef.h>
#include "../lib/string.h"
#include "../mm/boot_info.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"

/* Room the heap may grow into above the program image. */
#define BRK_MAX (256ULL * 1024 * 1024)

/* Maps a fresh stack and lays out argv in its top page, SysV style:
 *   rsp -> argc, argv[0..argc-1], NULL, (padding), strings...
 * with rsp 16-byte aligned, which is what crt0's _start expects. */
static int build_stack(uint64_t cr3, int argc, const char *const argv[], uint64_t *rsp_out) {
    uint64_t top_page = 0;
    for (uint64_t va = USER_STACK_TOP - USER_STACK_SIZE; va < USER_STACK_TOP; va += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -ENOMEM;
        vmm_map_user(cr3, va, phys, VMM_PRESENT | VMM_WRITABLE | VMM_NX);
        top_page = phys;
    }

    uint64_t page_base = USER_STACK_TOP - PAGE_SIZE;
    uint8_t *kpage = phys_to_virt(top_page);
    uint64_t sp = USER_STACK_TOP;
    uint64_t uargv[ARGV_MAX];

    for (int i = argc - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        if (USER_STACK_TOP - sp + len > ARGS_MAX) return -E2BIG;
        sp -= len;
        memcpy(kpage + (sp - page_base), argv[i], len);
        uargv[i] = sp;
    }

    uint64_t words = (uint64_t)argc + 2; /* argc, argv[], NULL */
    sp &= ~15ULL;
    if (words % 2) sp -= 8;
    sp -= words * 8;
    if (USER_STACK_TOP - sp > PAGE_SIZE) return -E2BIG;

    uint64_t *out = (uint64_t *)(kpage + (sp - page_base));
    out[0] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) out[1 + i] = uargv[i];
    out[1 + argc] = 0;

    *rsp_out = sp;
    return 0;
}

/* Builds a complete new address space for `path`: program image plus an
 * argv-carrying stack. Nothing is changed on failure. */
static int build_image(const char *path, int argc, const char *const argv[],
                       uint64_t *cr3_out, uint64_t *rip, uint64_t *rsp, uint64_t *image_end) {
    if (argc < 0 || argc > ARGV_MAX - 1) return -E2BIG;
    uint64_t cr3 = vmm_create_address_space();
    if (!cr3) return -ENOMEM;

    int err = elf_load(path, cr3, rip, image_end);
    if (!err) err = build_stack(cr3, argc, argv, rsp);
    if (err) {
        vmm_destroy_address_space(cr3);
        return err;
    }
    *cr3_out = cr3;
    return 0;
}

static const char *basename(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1]) b = p + 1;
    }
    return b;
}

struct task *process_spawn(const char *path, int argc, const char *const argv[]) {
    uint64_t cr3, rip, rsp, image_end;
    int err = build_image(path, argc, argv, &cr3, &rip, &rsp, &image_end);
    if (err) {
        kprintf("ATOS: failed to start %s: error %d\n", path, err);
        return NULL;
    }
    struct task *t = task_create_user_space(basename(path), cr3, rip, rsp);
    if (!t) {
        vmm_destroy_address_space(cr3);
        return NULL;
    }
    t->brk_start = t->brk = image_end;
    task_open_console_fds(t);
    task_start(t);
    return t;
}

int process_exec(struct registers *regs, const char *path, int argc, const char *const argv[]) {
    uint64_t cr3, rip, rsp, image_end;
    int err = build_image(path, argc, argv, &cr3, &rip, &rsp, &image_end);
    if (err) return err;

    /* Point of no return. Load the new tables before freeing the old ones:
     * we're running on them until the CR3 write (kernel stack and code are
     * in the shared upper half, so both views work for this code). */
    struct task *t = sched_current();
    uint64_t old_cr3 = t->cr3;
    t->cr3 = cr3;
    asm volatile("mov %0, %%cr3" ::"r"(cr3) : "memory");
    vmm_destroy_address_space(old_cr3);

    t->brk_start = t->brk = image_end;
    task_set_name(t, basename(path));

    /* A clean slate: the new program must see nothing of the old one's
     * registers. cs/ss already hold the user selectors. */
    memset(regs, 0, offsetof(struct registers, int_no));
    regs->rip = rip;
    regs->rsp = rsp;
    regs->rflags = 0x202; /* IF */
    fpu_reset();
    return 0;
}

uint64_t process_brk(uint64_t addr) {
    struct task *t = sched_current();
    if (addr < t->brk_start || addr > t->brk_start + BRK_MAX) return t->brk;

    uint64_t old_end = (t->brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    uint64_t new_end = (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t va = old_end; va < new_end; va += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) {
            /* Roll back to the old break so a failed grow maps nothing. */
            for (uint64_t undo = old_end; undo < va; undo += PAGE_SIZE) {
                pmm_free_page(vmm_unmap_user(t->cr3, undo));
            }
            return t->brk;
        }
        vmm_map_user(t->cr3, va, phys, VMM_PRESENT | VMM_WRITABLE | VMM_NX);
    }
    for (uint64_t va = new_end; va < old_end; va += PAGE_SIZE) {
        uint64_t phys = vmm_unmap_user(t->cr3, va);
        if (phys) pmm_free_page(phys);
    }
    t->brk = addr;
    return t->brk;
}

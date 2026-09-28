#include "elf.h"
#include "../fs/vfs.h"
#include "../lib/string.h"
#include "../mm/boot_info.h"
#include "../mm/pmm.h"
#include "../mm/vmm.h"
#include "../sched/sched.h"

struct elf64_ehdr {
    uint8_t  ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} __attribute__((packed));

#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define ET_EXEC     2
#define EM_X86_64   62
#define PT_LOAD     1
#define PF_X        1
#define PF_W        2
#define MAX_PHDRS   16

/* Program images may not reach up into the stack region. */
#define USER_IMAGE_LIMIT (USER_STACK_TOP - USER_STACK_SIZE - PAGE_SIZE)

static int read_exact(struct file *f, uint64_t offset, void *buf, uint64_t len) {
    if (vfs_seek(f, (int64_t)offset, SEEK_SET) < 0) return -ENOEXEC;
    uint8_t *p = buf;
    while (len) {
        int64_t n = vfs_read(f, p, len);
        if (n <= 0) return n < 0 ? (int)n : -ENOEXEC; /* truncated file */
        p += n;
        len -= (uint64_t)n;
    }
    return 0;
}

static int load_segment(struct file *f, uint64_t cr3, const struct elf64_phdr *ph) {
    uint64_t start = ph->vaddr & ~(PAGE_SIZE - 1);
    uint64_t end = ph->vaddr + ph->memsz;

    uint64_t want = VMM_PRESENT;
    if (ph->flags & PF_W) want |= VMM_WRITABLE;
    if (!(ph->flags & PF_X)) want |= VMM_NX;

    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        uint64_t flags;
        uint64_t phys = vmm_user_lookup(cr3, va, &flags);
        if (phys) {
            /* Two segments sharing a page: the page gets the union of their
             * rights (a linker script with page-aligned segments avoids it). */
            uint64_t merged = VMM_PRESENT | ((flags | want) & VMM_WRITABLE);
            if ((flags & VMM_NX) && (want & VMM_NX)) merged |= VMM_NX;
            vmm_map_user(cr3, va, phys, merged);
        } else {
            phys = pmm_alloc_page(); /* zeroed: covers .bss and alignment gaps */
            if (!phys) return -ENOMEM;
            vmm_map_user(cr3, va, phys, want);
        }
    }

    /* Copy the file-backed part a page at a time, straight into each
     * page through the HHDM -- no big intermediate buffer. */
    uint64_t copied = 0;
    while (copied < ph->filesz) {
        uint64_t va = ph->vaddr + copied;
        uint64_t in_page = va & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - in_page;
        if (chunk > ph->filesz - copied) chunk = ph->filesz - copied;
        uint64_t phys = vmm_user_lookup(cr3, va & ~(PAGE_SIZE - 1), NULL);
        int err = read_exact(f, ph->offset + copied, (uint8_t *)phys_to_virt(phys) + in_page, chunk);
        if (err) return err;
        copied += chunk;
    }
    return 0;
}

int elf_load(const char *path, uint64_t cr3, uint64_t *entry, uint64_t *image_end) {
    struct file *f;
    int err = vfs_open(path, O_RDONLY, &f);
    if (err) return err;
    if (f->vn->type != ATOS_TYPE_FILE) {
        file_close(f);
        return -EISDIR;
    }

    struct elf64_ehdr eh;
    struct elf64_phdr ph[MAX_PHDRS];
    err = read_exact(f, 0, &eh, sizeof(eh));
    if (err) goto out;

    err = -ENOEXEC;
    if (memcmp(eh.ident, "\x7f" "ELF", 4) != 0 || eh.ident[4] != ELFCLASS64 ||
        eh.ident[5] != ELFDATA2LSB || eh.type != ET_EXEC || eh.machine != EM_X86_64 ||
        eh.phentsize != sizeof(struct elf64_phdr) || eh.phnum == 0 || eh.phnum > MAX_PHDRS) {
        goto out;
    }
    err = read_exact(f, eh.phoff, ph, (uint64_t)eh.phnum * sizeof(struct elf64_phdr));
    if (err) goto out;

    uint64_t top = 0;
    for (unsigned i = 0; i < eh.phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
        /* All checks before any arithmetic can wrap or land in the kernel. */
        if (ph[i].filesz > ph[i].memsz || ph[i].vaddr < PAGE_SIZE ||
            ph[i].memsz > USER_IMAGE_LIMIT || ph[i].vaddr > USER_IMAGE_LIMIT - ph[i].memsz) {
            err = -ENOEXEC;
            goto out;
        }
        err = load_segment(f, cr3, &ph[i]);
        if (err) goto out;
        uint64_t seg_end = (ph[i].vaddr + ph[i].memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (seg_end > top) top = seg_end;
    }

    err = -ENOEXEC;
    if (top == 0 || eh.entry < PAGE_SIZE || eh.entry >= top) goto out;
    *entry = eh.entry;
    *image_end = top;
    err = 0;

out:
    file_close(f);
    return err;
}

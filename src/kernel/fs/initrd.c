#include "initrd.h"
#include "vfs.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include <limine.h>

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST,
    .revision = 0,
};

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6]; /* "ustar\0" (POSIX) or "ustar " (old GNU) */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed));

#define TAR_BLOCK 512
#define TAR_TYPE_FILE  '0'
#define TAR_TYPE_AFILE '\0' /* pre-POSIX regular file */
#define TAR_TYPE_DIR   '5'

/* One file or directory. `vn` comes first so a vnode pointer converts
 * straight back to its node. Nodes are never freed: the initrd lives as
 * long as the kernel does, hence no release op. */
struct tnode {
    struct vnode vn;
    char name[ATOS_NAME_MAX];
    const uint8_t *data;
    struct tnode *children, *last_child, *sibling;
};

static const struct vnode_ops file_ops;
static const struct vnode_ops dir_ops;
static struct tnode root;

static uint64_t parse_octal(const char *s, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i] >= '0' && s[i] <= '7'; i++) v = v * 8 + (uint64_t)(s[i] - '0');
    return v;
}

static int checksum_ok(const struct tar_header *h) {
    /* Sum of all header bytes, with the checksum field counted as spaces. */
    const uint8_t *b = (const uint8_t *)h;
    uint64_t sum = 0;
    for (size_t i = 0; i < TAR_BLOCK; i++) {
        int in_field = i >= offsetof(struct tar_header, checksum) &&
                       i < offsetof(struct tar_header, checksum) + sizeof(h->checksum);
        sum += in_field ? ' ' : b[i];
    }
    return sum == parse_octal(h->checksum, sizeof(h->checksum));
}

static struct tnode *find_child(struct tnode *dir, const char *name, size_t len) {
    for (struct tnode *c = dir->children; c; c = c->sibling) {
        if (strlen(c->name) == len && memcmp(c->name, name, len) == 0) return c;
    }
    return NULL;
}

static struct tnode *add_child(struct tnode *dir, const char *name, size_t len, uint32_t type) {
    struct tnode *n = kmalloc(sizeof(*n));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    memcpy(n->name, name, len);
    n->vn.type = type;
    n->vn.ops = type == ATOS_TYPE_DIR ? &dir_ops : &file_ops;
    n->vn.refcount = 1; /* the tree's own reference: never dropped */
    if (dir->last_child) dir->last_child->sibling = n;
    else dir->children = n;
    dir->last_child = n;
    return n;
}

/* Inserts one archive entry, creating missing parent directories the
 * way `mkdir -p` would (archives don't always list them explicitly). */
static void insert(const char *path, uint32_t type, const uint8_t *data, uint64_t size) {
    struct tnode *dir = &root;
    const char *p = path;
    for (;;) {
        while (*p == '/') p++;
        if (!*p) return; /* the root itself, or a trailing slash */
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len >= ATOS_NAME_MAX) {
            kprintf("ATOS: initrd: name too long in %s, skipped\n", path);
            return;
        }
        if (len == 1 && start[0] == '.') continue;

        while (*p == '/') p++;
        int last = *p == '\0';
        struct tnode *n = find_child(dir, start, len);
        if (!n) n = add_child(dir, start, len, last ? type : ATOS_TYPE_DIR);
        if (!n) return;
        if (last) {
            if (type == ATOS_TYPE_FILE) {
                n->data = data;
                n->vn.size = size;
            }
            return;
        }
        if (n->vn.type != ATOS_TYPE_DIR) return; /* a file used as a directory */
        dir = n;
    }
}

static unsigned parse_archive(const uint8_t *base, uint64_t size) {
    unsigned entries = 0;
    uint64_t off = 0;
    while (off + TAR_BLOCK <= size) {
        const struct tar_header *h = (const struct tar_header *)(base + off);
        if (h->name[0] == '\0') break; /* end-of-archive zero block */
        if (memcmp(h->magic, "ustar", 5) != 0 || !checksum_ok(h)) {
            kprintf("ATOS: initrd: corrupt header at offset %lu, stopping\n", off);
            break;
        }

        uint64_t fsize = parse_octal(h->size, sizeof(h->size));
        const uint8_t *data = base + off + TAR_BLOCK;
        if (off + TAR_BLOCK + fsize > size) {
            kprintf("ATOS: initrd: truncated entry at offset %lu, stopping\n", off);
            break;
        }

        /* ustar splits long paths into prefix + "/" + name. */
        char path[sizeof(h->prefix) + 1 + sizeof(h->name) + 1];
        size_t len = 0;
        size_t plen = strnlen(h->prefix, sizeof(h->prefix));
        if (plen) {
            memcpy(path, h->prefix, plen);
            len = plen;
            path[len++] = '/';
        }
        size_t nlen = strnlen(h->name, sizeof(h->name));
        memcpy(path + len, h->name, nlen);
        path[len + nlen] = '\0';

        if (h->typeflag == TAR_TYPE_FILE || h->typeflag == TAR_TYPE_AFILE) {
            insert(path, ATOS_TYPE_FILE, data, fsize);
            entries++;
        } else if (h->typeflag == TAR_TYPE_DIR) {
            insert(path, ATOS_TYPE_DIR, NULL, 0);
            entries++;
        } /* links, devices, etc. aren't supported and are skipped */

        off += TAR_BLOCK + (fsize + TAR_BLOCK - 1) / TAR_BLOCK * TAR_BLOCK;
    }
    return entries;
}

static int64_t initrd_read(struct vnode *vn, uint64_t offset, void *buf, uint64_t len) {
    struct tnode *n = (struct tnode *)vn;
    if (offset >= vn->size) return 0;
    if (len > vn->size - offset) len = vn->size - offset;
    memcpy(buf, n->data + offset, len);
    return (int64_t)len;
}

static int initrd_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct tnode *c = find_child((struct tnode *)dir, name, strlen(name));
    if (!c) return -ENOENT;
    vnode_ref(&c->vn);
    *out = &c->vn;
    return 0;
}

static int initrd_readdir(struct vnode *dir, uint64_t index, struct atos_dirent *out) {
    struct tnode *c = ((struct tnode *)dir)->children;
    while (c && index--) c = c->sibling;
    if (!c) return -ENOENT;
    memset(out, 0, sizeof(*out));
    memcpy(out->name, c->name, strlen(c->name) + 1);
    out->type = c->vn.type;
    out->size = c->vn.size;
    return 0;
}

static const struct vnode_ops file_ops = {.read = initrd_read};
static const struct vnode_ops dir_ops = {.lookup = initrd_lookup, .readdir = initrd_readdir};

void initrd_init(void) {
    struct limine_module_response *resp = module_request.response;
    if (!resp || resp->module_count == 0) {
        kprintf("ATOS: initrd: no module loaded, / will be empty\n");
    }

    root.vn.type = ATOS_TYPE_DIR;
    root.vn.ops = &dir_ops;
    root.vn.refcount = 1;

    if (resp && resp->module_count > 0) {
        struct limine_file *mod = resp->modules[0];
        unsigned entries = parse_archive(mod->address, mod->size);
        kprintf("ATOS: initrd: %s, %lu KiB, %u entries\n", mod->path, mod->size / 1024, entries);
    }
    vfs_mount("/", &root.vn);
}

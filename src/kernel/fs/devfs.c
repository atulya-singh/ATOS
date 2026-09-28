#include "devfs.h"
#include "vfs.h"
#include "../dev/console.h"
#include "../dev/keyboard.h"
#include "../lib/string.h"

/* Device nodes are static and never freed, so no release op. */

static int64_t console_read(struct vnode *vn, uint64_t offset, void *buf, uint64_t len) {
    (void)vn;
    (void)offset;
    if (len == 0) return 0;
    /* Raw-mode tty semantics: block for one key, then take whatever else
     * is already queued. Echo and line editing are the reader's job. */
    char *p = buf;
    p[0] = keyboard_getc();
    uint64_t n = 1;
    int c;
    while (n < len && (c = keyboard_try_getc()) >= 0) p[n++] = (char)c;
    return (int64_t)n;
}

static int64_t console_write(struct vnode *vn, uint64_t offset, const void *buf, uint64_t len) {
    (void)vn;
    (void)offset;
    console_write_chars(buf, len);
    return (int64_t)len;
}

static int64_t null_read(struct vnode *vn, uint64_t offset, void *buf, uint64_t len) {
    (void)vn, (void)offset, (void)buf, (void)len;
    return 0;
}

static int64_t null_write(struct vnode *vn, uint64_t offset, const void *buf, uint64_t len) {
    (void)vn, (void)offset, (void)buf;
    return (int64_t)len;
}

static const struct vnode_ops console_ops = {.read = console_read, .write = console_write};
static const struct vnode_ops null_ops = {.read = null_read, .write = null_write};

static struct vnode console_node = {.type = ATOS_TYPE_CHARDEV, .ops = &console_ops, .refcount = 1};
static struct vnode null_node = {.type = ATOS_TYPE_CHARDEV, .ops = &null_ops, .refcount = 1};

static const struct {
    const char *name;
    struct vnode *node;
} devices[] = {
    {"console", &console_node},
    {"null", &null_node},
};
#define DEVICE_COUNT (sizeof(devices) / sizeof(devices[0]))

static int devfs_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    (void)dir;
    size_t len = strlen(name);
    for (size_t i = 0; i < DEVICE_COUNT; i++) {
        if (strlen(devices[i].name) == len && memcmp(devices[i].name, name, len) == 0) {
            vnode_ref(devices[i].node);
            *out = devices[i].node;
            return 0;
        }
    }
    return -ENOENT;
}

static int devfs_readdir(struct vnode *dir, uint64_t index, struct atos_dirent *out) {
    (void)dir;
    if (index >= DEVICE_COUNT) return -ENOENT;
    memset(out, 0, sizeof(*out));
    memcpy(out->name, devices[index].name, strlen(devices[index].name) + 1);
    out->type = ATOS_TYPE_CHARDEV;
    return 0;
}

static const struct vnode_ops root_ops = {.lookup = devfs_lookup, .readdir = devfs_readdir};
static struct vnode root_node = {.type = ATOS_TYPE_DIR, .ops = &root_ops, .refcount = 1};

void devfs_init(void) {
    vfs_mount("/dev", &root_node);
}

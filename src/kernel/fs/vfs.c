#include "vfs.h"
#include "../lib/kprintf.h"
#include "../lib/path.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"

#define MAX_MOUNTS 8

static struct {
    char path[VFS_PATH_MAX]; /* normalized */
    size_t len;
    struct vnode *root;
} mounts[MAX_MOUNTS];
static unsigned mount_count;

void vnode_ref(struct vnode *vn) {
    __atomic_add_fetch(&vn->refcount, 1, __ATOMIC_SEQ_CST);
}

void vnode_release(struct vnode *vn) {
    if (__atomic_sub_fetch(&vn->refcount, 1, __ATOMIC_SEQ_CST) == 0 && vn->ops->release) {
        vn->ops->release(vn);
    }
}

int vfs_mount(const char *path, struct vnode *root) {
    if (mount_count == MAX_MOUNTS) return -ENOMEM;
    int err = path_normalize(path, mounts[mount_count].path);
    if (err) return err;
    mounts[mount_count].len = strlen(mounts[mount_count].path);
    mounts[mount_count].root = root;
    vnode_ref(root);
    mount_count++;
    kprintf("ATOS: vfs: mounted %s\n", mounts[mount_count - 1].path);
    return 0;
}

/* Walks a normalized path from the deepest mount covering it. */
static int resolve(const char *norm, struct vnode **out) {
    int best = -1;
    for (unsigned i = 0; i < mount_count; i++) {
        size_t mlen = mounts[i].len;
        int covers = (mlen == 1) /* "/" covers everything */
                  || (memcmp(norm, mounts[i].path, mlen) == 0 &&
                      (norm[mlen] == '/' || norm[mlen] == '\0'));
        if (covers && (best < 0 || mlen > mounts[best].len)) best = (int)i;
    }
    if (best < 0) return -ENOENT;

    struct vnode *vn = mounts[best].root;
    vnode_ref(vn);
    const char *p = norm + (mounts[best].len == 1 ? 0 : mounts[best].len);

    char name[ATOS_NAME_MAX];
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);
        memcpy(name, start, clen);
        name[clen] = '\0';

        if (vn->type != ATOS_TYPE_DIR || !vn->ops->lookup) {
            vnode_release(vn);
            return -ENOTDIR;
        }
        struct vnode *child;
        int err = vn->ops->lookup(vn, name, &child);
        vnode_release(vn);
        if (err) return err;
        vn = child;
    }
    *out = vn;
    return 0;
}

int vfs_lookup(const char *path, struct vnode **out) {
    char norm[VFS_PATH_MAX];
    int err = path_normalize(path, norm);
    if (err) return err;
    return resolve(norm, out);
}

/* Whether a normalized path is some filesystem's mount point, which can
 * be neither removed nor renamed. */
static int is_mount_point(const char *norm) {
    for (unsigned i = 0; i < mount_count; i++) {
        if (strcmp(norm, mounts[i].path) == 0) return 1;
    }
    return 0;
}

/* Resolves the parent directory of a normalized path and points *leaf at
 * its last component. -EINVAL for "/" itself. */
static int resolve_parent(const char *norm, struct vnode **dir, const char **leaf) {
    char parent[VFS_PATH_MAX];
    size_t len = strlen(norm);
    size_t slash = len;
    while (slash > 0 && norm[slash - 1] != '/') slash--;
    if (slash == 0 || slash == len) return -EINVAL;
    *leaf = norm + slash;

    memcpy(parent, norm, slash);
    parent[slash > 1 ? slash - 1 : 1] = '\0';
    int err = resolve(parent, dir);
    if (err) return err;
    if ((*dir)->type != ATOS_TYPE_DIR) {
        vnode_release(*dir);
        return -ENOTDIR;
    }
    return 0;
}

/* Creates the last component of a normalized path inside its parent. */
static int create_at(const char *norm, struct vnode **out) {
    struct vnode *dir;
    const char *leaf;
    int err = resolve_parent(norm, &dir, &leaf);
    if (err) return err;
    err = dir->ops->create ? dir->ops->create(dir, leaf, out) : -EROFS;
    vnode_release(dir);
    return err;
}

int vfs_mkdir(const char *path) {
    char norm[VFS_PATH_MAX];
    int err = path_normalize(path, norm);
    if (err) return err;
    if (is_mount_point(norm)) return -EEXIST;
    struct vnode *dir;
    const char *leaf;
    err = resolve_parent(norm, &dir, &leaf);
    if (err) return err;
    err = dir->ops->mkdir ? dir->ops->mkdir(dir, leaf) : -EROFS;
    vnode_release(dir);
    return err;
}

static int remove_path(const char *path, int want_dir) {
    char norm[VFS_PATH_MAX];
    int err = path_normalize(path, norm);
    if (err) return err;
    if (is_mount_point(norm)) return -EBUSY;
    struct vnode *dir;
    const char *leaf;
    err = resolve_parent(norm, &dir, &leaf);
    if (err) return err;
    err = dir->ops->remove ? dir->ops->remove(dir, leaf, want_dir) : -EROFS;
    vnode_release(dir);
    return err;
}

int vfs_unlink(const char *path) { return remove_path(path, 0); }
int vfs_rmdir(const char *path) { return remove_path(path, 1); }

int vfs_rename(const char *old_path, const char *new_path) {
    char old_norm[VFS_PATH_MAX], new_norm[VFS_PATH_MAX];
    int err = path_normalize(old_path, old_norm);
    if (!err) err = path_normalize(new_path, new_norm);
    if (err) return err;
    if (is_mount_point(old_norm) || is_mount_point(new_norm)) return -EBUSY;

    struct vnode *old_dir, *new_dir;
    const char *old_leaf, *new_leaf;
    err = resolve_parent(old_norm, &old_dir, &old_leaf);
    if (err) return err;
    err = resolve_parent(new_norm, &new_dir, &new_leaf);
    if (err) {
        vnode_release(old_dir);
        return err;
    }
    /* Both ends must be on the same filesystem. (Moving a directory into
     * its own subtree is the filesystem's to catch: FAT names are
     * case-insensitive, so a lexical prefix check here would miss some.) */
    if (!old_dir->ops->rename) err = -EROFS;
    else if (old_dir->ops != new_dir->ops || old_dir->fs_data != new_dir->fs_data) err = -EXDEV;
    else err = old_dir->ops->rename(old_dir, old_leaf, new_dir, new_leaf);
    vnode_release(old_dir);
    vnode_release(new_dir);
    return err;
}

int vfs_open(const char *path, int flags, struct file **out) {
    char norm[VFS_PATH_MAX];
    int err = path_normalize(path, norm);
    if (err) return err;

    struct vnode *vn;
    err = resolve(norm, &vn);
    if (err == -ENOENT && (flags & O_CREAT)) err = create_at(norm, &vn);
    if (err) return err;

    int acc = flags & O_ACCMODE;
    if (vn->type == ATOS_TYPE_DIR && acc != O_RDONLY) {
        vnode_release(vn);
        return -EISDIR;
    }
    if ((flags & O_TRUNC) && acc != O_RDONLY && vn->type == ATOS_TYPE_FILE) {
        err = vn->ops->truncate ? vn->ops->truncate(vn, 0) : -EROFS;
        if (err) {
            vnode_release(vn);
            return err;
        }
    }

    struct file *f = kmalloc(sizeof(*f));
    if (!f) {
        vnode_release(vn);
        return -ENOMEM;
    }
    f->vn = vn;
    f->offset = 0;
    f->flags = flags;
    f->refcount = 1;
    *out = f;
    return 0;
}

void file_ref(struct file *f) {
    __atomic_add_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST);
}

void file_close(struct file *f) {
    if (__atomic_sub_fetch(&f->refcount, 1, __ATOMIC_SEQ_CST) != 0) return;
    vnode_release(f->vn);
    kfree(f);
}

int64_t vfs_read(struct file *f, void *buf, uint64_t len) {
    if ((f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    if (f->vn->type == ATOS_TYPE_DIR) return -EISDIR;
    if (!f->vn->ops->read) return -EINVAL;
    int64_t n = f->vn->ops->read(f->vn, f->offset, buf, len);
    if (n > 0 && f->vn->type == ATOS_TYPE_FILE) f->offset += (uint64_t)n;
    return n;
}

int64_t vfs_write(struct file *f, const void *buf, uint64_t len) {
    if ((f->flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    if (!f->vn->ops->write) return -EROFS;
    if ((f->flags & O_APPEND) && f->vn->type == ATOS_TYPE_FILE) f->offset = f->vn->size;
    int64_t n = f->vn->ops->write(f->vn, f->offset, buf, len);
    if (n > 0 && f->vn->type == ATOS_TYPE_FILE) f->offset += (uint64_t)n;
    return n;
}

int64_t vfs_seek(struct file *f, int64_t offset, int whence) {
    if (f->vn->type != ATOS_TYPE_FILE) return -ESPIPE;
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)f->offset; break;
    case SEEK_END: base = (int64_t)f->vn->size; break;
    default: return -EINVAL;
    }
    if (base + offset < 0) return -EINVAL;
    f->offset = (uint64_t)(base + offset);
    return (int64_t)f->offset;
}

int vfs_readdir(struct file *f, uint64_t index, struct atos_dirent *out) {
    if (f->vn->type != ATOS_TYPE_DIR || !f->vn->ops->readdir) return -ENOTDIR;
    return f->vn->ops->readdir(f->vn, index, out);
}

void vfs_stat(struct file *f, struct atos_stat *out) {
    out->type = f->vn->type;
    out->reserved = 0;
    out->size = f->vn->size;
}

/* --- per-task fd tables. Only the owning task touches its own table
 * (fork copies it before the child ever runs), so no locking needed. --- */

int fd_install(struct task *t, struct file *f) {
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (!t->fds[fd]) {
            t->fds[fd] = f;
            return fd;
        }
    }
    return -EMFILE;
}

struct file *fd_get(struct task *t, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return NULL;
    return t->fds[fd];
}

int fd_close(struct task *t, int fd) {
    struct file *f = fd_get(t, fd);
    if (!f) return -EBADF;
    t->fds[fd] = NULL;
    file_close(f);
    return 0;
}

int fd_dup2(struct task *t, int oldfd, int newfd) {
    struct file *f = fd_get(t, oldfd);
    if (!f || newfd < 0 || newfd >= MAX_FDS) return -EBADF;
    if (oldfd == newfd) return newfd;
    file_ref(f);
    if (t->fds[newfd]) file_close(t->fds[newfd]);
    t->fds[newfd] = f;
    return newfd;
}

void fd_inherit(struct task *child, struct task *parent) {
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (parent->fds[fd]) {
            file_ref(parent->fds[fd]);
            child->fds[fd] = parent->fds[fd];
        }
    }
}

void fd_close_all(struct task *t) {
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (t->fds[fd]) fd_close(t, fd);
    }
}

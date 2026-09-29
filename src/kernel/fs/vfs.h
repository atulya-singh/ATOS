#pragma once
#include <atos/abi.h>
#include <stddef.h>
#include <stdint.h>

/* A file, directory, or device as the VFS sees it. Filesystems allocate
 * them and fill in `ops`; the VFS never looks inside `fs_data`.
 *
 * Lifetime: every vnode handed out by lookup/create carries one reference
 * for the caller, dropped with vnode_release. When the count reaches zero,
 * ops->release (if any) lets the filesystem free it. Filesystems whose
 * vnodes live forever (initrd, devfs) leave release NULL. */
struct vnode;

struct vnode_ops {
    /* Byte-granular I/O at `offset`; return bytes moved, 0 at EOF, or
     * -errno. `buf` may be a validated user pointer. */
    int64_t (*read)(struct vnode *vn, uint64_t offset, void *buf, uint64_t len);
    int64_t (*write)(struct vnode *vn, uint64_t offset, const void *buf, uint64_t len);

    /* Directories only. lookup/create return a referenced vnode. readdir
     * returns -ENOENT once `index` is past the last entry. */
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int (*readdir)(struct vnode *dir, uint64_t index, struct atos_dirent *out);
    int (*create)(struct vnode *dir, const char *name, struct vnode **out);
    int (*mkdir)(struct vnode *dir, const char *name);
    /* Removes a file (want_dir 0) or an empty directory (want_dir 1):
     * -EISDIR / -ENOTDIR on a mismatch, -ENOTEMPTY, -EBUSY while open. */
    int (*remove)(struct vnode *dir, const char *name, int want_dir);
    /* Moves `name` in `dir` to `new_name` in `new_dir` (same filesystem;
     * the VFS checks). An existing file target is replaced. */
    int (*rename)(struct vnode *dir, const char *name, struct vnode *new_dir, const char *new_name);

    int (*truncate)(struct vnode *vn, uint64_t size);
    void (*release)(struct vnode *vn);
};

struct vnode {
    uint32_t type; /* ATOS_TYPE_* */
    uint64_t size;
    const struct vnode_ops *ops;
    void *fs_data;
    int refcount;
};

void vnode_ref(struct vnode *vn);
void vnode_release(struct vnode *vn);

/* Attaches `root` at an absolute path. Lookups resolve through the mount
 * with the longest matching prefix, so mounts can nest (/ and /dev). */
int vfs_mount(const char *path, struct vnode *root);

/* Resolves an absolute path ("." and ".." are handled lexically) to a
 * referenced vnode. */
int vfs_lookup(const char *path, struct vnode **out);

/* An open file: a vnode plus the per-open offset and access mode. Shared
 * (refcounted) between file descriptors after fork or dup2. */
struct file {
    struct vnode *vn;
    uint64_t offset;
    int flags;
    int refcount;
};

int vfs_open(const char *path, int flags, struct file **out);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_rmdir(const char *path);
int vfs_rename(const char *old_path, const char *new_path);
void file_ref(struct file *f);
void file_close(struct file *f);

int64_t vfs_read(struct file *f, void *buf, uint64_t len);
int64_t vfs_write(struct file *f, const void *buf, uint64_t len);
int64_t vfs_seek(struct file *f, int64_t offset, int whence);
int vfs_readdir(struct file *f, uint64_t index, struct atos_dirent *out);
void vfs_stat(struct file *f, struct atos_stat *out);

/* --- per-task file descriptor tables --- */

#define MAX_FDS 16
struct task;

/* Installs `f` (taking over the caller's reference) at the lowest free fd. */
int fd_install(struct task *t, struct file *f);
struct file *fd_get(struct task *t, int fd);
int fd_close(struct task *t, int fd);
/* Makes `newfd` refer to the same open file as `oldfd`. */
int fd_dup2(struct task *t, int oldfd, int newfd);
/* Gives `child` its own references to every file `parent` has open. */
void fd_inherit(struct task *child, struct task *parent);
void fd_close_all(struct task *t);

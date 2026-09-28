#include "fat.h"
#include "vfs.h"
#include "../dev/block.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"

/* FAT32 on a whole block device (no partition table). Names come from
 * VFAT long-name entries when present, otherwise from the 8.3 name with
 * its NT lowercase flags applied. Lookups are case-insensitive, as on
 * every other FAT implementation. One mutex per volume serializes all
 * access, which also protects the shared FAT-sector cache and cluster
 * buffer. */

struct bpb {
    uint8_t  jump[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;   /* 0 on FAT32 */
    uint16_t total_sectors16;
    uint8_t  media;
    uint16_t fat_size16;     /* 0 on FAT32 */
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors32;
    uint32_t fat_size32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
    uint16_t backup_boot_sector;
} __attribute__((packed));

struct raw_dirent {
    char     name[11]; /* 8.3, space padded */
    uint8_t  attr;
    uint8_t  ntres;    /* NT case flags */
    uint8_t  ctime_tenth;
    uint16_t ctime, cdate, adate;
    uint16_t cluster_hi;
    uint16_t mtime, mdate;
    uint16_t cluster_lo;
    uint32_t size;
} __attribute__((packed));

#define ATTR_READ_ONLY 0x01
#define ATTR_HIDDEN    0x02
#define ATTR_SYSTEM    0x04
#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_ARCHIVE   0x20
#define ATTR_LFN       0x0F

#define NTRES_LOWER_BASE 0x08
#define NTRES_LOWER_EXT  0x10

#define DIRENT_END     0x00
#define DIRENT_DELETED 0xE5

#define FAT_ENTRY_MASK 0x0FFFFFFF
#define FAT_EOC_MIN    0x0FFFFFF8 /* end of chain */
#define FAT_BAD        0x0FFFFFF7

struct fat_fs {
    struct block_device *dev;
    uint32_t sectors_per_cluster;
    uint32_t cluster_bytes;
    uint32_t fat_start;   /* LBA of the first FAT */
    uint32_t fat_sectors; /* per FAT copy */
    uint32_t num_fats;
    uint32_t data_start;  /* LBA of cluster 2 */
    uint32_t cluster_count;
    uint32_t root_cluster;

    struct mutex lock;
    uint8_t fat_cache[BLOCK_SECTOR_SIZE];
    uint32_t fat_cache_lba; /* 0 = empty (the FAT never starts at LBA 0) */
    uint8_t *cluster_buf;
};

/* One open file or directory. `vn` first so vnode pointers convert back. */
struct fat_node {
    struct vnode vn;
    struct fat_fs *fs;
    uint32_t first_cluster; /* 0 for an empty file */

    /* Where this node's 8.3 entry lives in its parent directory, so size
     * and first-cluster changes can be written back. Unused for the root. */
    uint32_t dirent_cluster;
    uint32_t dirent_offset;

    /* Last position resolved in the cluster chain, so sequential reads
     * walk the FAT once instead of once per cluster (O(n) not O(n^2)). */
    uint32_t cursor_index;
    uint32_t cursor_cluster;
};

static const struct vnode_ops file_ops;
static const struct vnode_ops dir_ops;

static uint64_t cluster_lba(struct fat_fs *fs, uint32_t cluster) {
    return fs->data_start + (uint64_t)(cluster - 2) * fs->sectors_per_cluster;
}

static int valid_cluster(struct fat_fs *fs, uint32_t c) {
    return c >= 2 && c < fs->cluster_count + 2;
}

/* Reads the FAT entry for `cluster` (the next cluster in its chain). */
static int fat_get(struct fat_fs *fs, uint32_t cluster, uint32_t *next) {
    uint64_t byte = (uint64_t)cluster * 4;
    uint32_t lba = fs->fat_start + (uint32_t)(byte / BLOCK_SECTOR_SIZE);
    if (fs->fat_cache_lba != lba) {
        int err = block_read(fs->dev, lba, 1, fs->fat_cache);
        if (err) {
            fs->fat_cache_lba = 0;
            return err;
        }
        fs->fat_cache_lba = lba;
    }
    uint32_t v;
    memcpy(&v, fs->fat_cache + byte % BLOCK_SECTOR_SIZE, 4);
    *next = v & FAT_ENTRY_MASK;
    return 0;
}

/* Finds the index-th cluster of a node's chain, walking forward from the
 * cached cursor when possible. -EIO on a chain that ends too early. */
static int node_cluster_at(struct fat_node *n, uint32_t index, uint32_t *out) {
    struct fat_fs *fs = n->fs;
    uint32_t i = 0, c = n->first_cluster;
    if (n->cursor_cluster && n->cursor_index <= index) {
        i = n->cursor_index;
        c = n->cursor_cluster;
    }
    while (i < index) {
        if (!valid_cluster(fs, c)) return -EIO;
        int err = fat_get(fs, c, &c);
        if (err) return err;
        i++;
    }
    if (!valid_cluster(fs, c)) return -EIO;
    n->cursor_index = index;
    n->cursor_cluster = c;
    *out = c;
    return 0;
}

static int read_cluster(struct fat_fs *fs, uint32_t cluster, void *buf) {
    return block_read(fs->dev, cluster_lba(fs, cluster), fs->sectors_per_cluster, buf);
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static int name_eq_nocase(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return *a == *b;
}

/* "FOO     TXT" -> "FOO.TXT" (or "foo.txt" per the NT case flags). */
static void short_name(const struct raw_dirent *d, char *out) {
    size_t n = 0;
    for (int i = 0; i < 8 && d->name[i] != ' '; i++) {
        out[n++] = (d->ntres & NTRES_LOWER_BASE) ? lower(d->name[i]) : d->name[i];
    }
    if (d->name[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && d->name[i] != ' '; i++) {
            out[n++] = (d->ntres & NTRES_LOWER_EXT) ? lower(d->name[i]) : d->name[i];
        }
    }
    out[n] = '\0';
    if (out[0] == 0x05) out[0] = (char)0xE5; /* escaped first byte */
}

static uint8_t short_name_checksum(const char name[11]) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++) sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + (uint8_t)name[i]);
    return sum;
}

/* One directory entry as dir_iterate presents it. */
struct entry {
    char name[ATOS_NAME_MAX];
    struct raw_dirent raw;
    uint32_t cluster; /* where the 8.3 entry lives */
    uint32_t offset;
};

/* Calls visit() for each live entry of the directory starting at
 * `dir_cluster` (skipping ".", "..", and the volume label) until it
 * returns nonzero, which dir_iterate then returns. 0 = ran off the end.
 * visit() must not do filesystem I/O: the entry it sees lives in
 * cluster_buf, which any I/O would overwrite. */
static int dir_iterate(struct fat_fs *fs, uint32_t dir_cluster,
                       int (*visit)(void *ctx, const struct entry *e), void *ctx) {
    /* VFAT long names: up to 20 entries of 13 UCS-2 characters each, stored
     * last-piece-first before the 8.3 entry they belong to. */
    char lfn[20 * 13 + 1];
    int lfn_pieces = 0;
    uint8_t lfn_checksum = 0;
    struct entry e;

    uint32_t c = dir_cluster;
    while (valid_cluster(fs, c)) {
        int err = read_cluster(fs, c, fs->cluster_buf);
        if (err) return err;
        for (uint32_t off = 0; off < fs->cluster_bytes; off += sizeof(struct raw_dirent)) {
            const struct raw_dirent *d = (const struct raw_dirent *)(fs->cluster_buf + off);
            uint8_t first = (uint8_t)d->name[0];
            if (first == DIRENT_END) return 0;
            if (first == DIRENT_DELETED) {
                lfn_pieces = 0;
                continue;
            }
            if (d->attr == ATTR_LFN) {
                const uint8_t *b = (const uint8_t *)d;
                int seq = b[0] & 0x1F;
                if (seq < 1 || seq > 20) {
                    lfn_pieces = 0;
                    continue;
                }
                if (b[0] & 0x40) { /* the last piece comes first */
                    lfn_pieces = seq;
                    lfn_checksum = b[13];
                    lfn[seq * 13] = '\0';
                }
                static const uint8_t char_offsets[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
                for (int i = 0; i < 13; i++) {
                    uint16_t ch = (uint16_t)(b[char_offsets[i]] | (b[char_offsets[i] + 1] << 8));
                    char out = ch == 0 ? '\0' : ch == 0xFFFF ? '\0' : ch < 128 ? (char)ch : '?';
                    lfn[(seq - 1) * 13 + i] = out;
                }
                continue;
            }
            if (d->attr & ATTR_VOLUME_ID) {
                lfn_pieces = 0;
                continue;
            }

            if (lfn_pieces && lfn_checksum == short_name_checksum(d->name) && lfn[0]) {
                size_t len = strnlen(lfn, sizeof(lfn));
                if (len >= ATOS_NAME_MAX) len = ATOS_NAME_MAX - 1;
                memcpy(e.name, lfn, len);
                e.name[len] = '\0';
            } else {
                short_name(d, e.name);
            }
            lfn_pieces = 0;
            if (strcmp(e.name, ".") == 0 || strcmp(e.name, "..") == 0) continue;

            e.raw = *d;
            e.cluster = c;
            e.offset = off;
            int stop = visit(ctx, &e);
            if (stop) return stop;
        }
        err = fat_get(fs, c, &c);
        if (err) return err;
    }
    return 0;
}

static struct fat_node *node_new(struct fat_fs *fs, const struct entry *e) {
    struct fat_node *n = kmalloc(sizeof(*n));
    if (!n) return NULL;
    memset(n, 0, sizeof(*n));
    n->fs = fs;
    n->first_cluster = ((uint32_t)e->raw.cluster_hi << 16) | e->raw.cluster_lo;
    n->dirent_cluster = e->cluster;
    n->dirent_offset = e->offset;
    int is_dir = (e->raw.attr & ATTR_DIRECTORY) != 0;
    n->vn.type = is_dir ? ATOS_TYPE_DIR : ATOS_TYPE_FILE;
    n->vn.size = is_dir ? 0 : e->raw.size;
    n->vn.ops = is_dir ? &dir_ops : &file_ops;
    n->vn.fs_data = fs;
    n->vn.refcount = 1;
    return n;
}

/* --- vnode ops --- */

static int64_t fat_read(struct vnode *vn, uint64_t offset, void *buf, uint64_t len) {
    struct fat_node *n = (struct fat_node *)vn;
    struct fat_fs *fs = n->fs;
    if (offset >= vn->size) return 0;
    if (len > vn->size - offset) len = vn->size - offset;

    mutex_lock(&fs->lock);
    uint64_t done = 0;
    int err = 0;
    while (done < len) {
        uint64_t pos = offset + done;
        uint32_t cluster;
        err = node_cluster_at(n, (uint32_t)(pos / fs->cluster_bytes), &cluster);
        if (err) break;
        err = read_cluster(fs, cluster, fs->cluster_buf);
        if (err) break;
        uint64_t in_cluster = pos % fs->cluster_bytes;
        uint64_t chunk = fs->cluster_bytes - in_cluster;
        if (chunk > len - done) chunk = len - done;
        memcpy((uint8_t *)buf + done, fs->cluster_buf + in_cluster, chunk);
        done += chunk;
    }
    mutex_unlock(&fs->lock);
    return done ? (int64_t)done : err;
}

struct lookup_ctx {
    const char *name;
    struct entry found;
};

static int lookup_visit(void *ctx, const struct entry *e) {
    struct lookup_ctx *lc = ctx;
    if (!name_eq_nocase(e->name, lc->name)) return 0;
    lc->found = *e;
    return 1;
}

static int fat_lookup(struct vnode *dir, const char *name, struct vnode **out) {
    struct fat_node *d = (struct fat_node *)dir;
    struct fat_fs *fs = d->fs;
    struct lookup_ctx lc = {.name = name};

    mutex_lock(&fs->lock);
    int r = dir_iterate(fs, d->first_cluster, lookup_visit, &lc);
    mutex_unlock(&fs->lock);
    if (r < 0) return r;
    if (r == 0) return -ENOENT;

    struct fat_node *n = node_new(fs, &lc.found);
    if (!n) return -ENOMEM;
    *out = &n->vn;
    return 0;
}

struct readdir_ctx {
    uint64_t want;
    uint64_t seen;
    struct atos_dirent *out;
};

static int readdir_visit(void *ctx, const struct entry *e) {
    struct readdir_ctx *rc = ctx;
    if (rc->seen++ != rc->want) return 0;
    memset(rc->out, 0, sizeof(*rc->out));
    memcpy(rc->out->name, e->name, strlen(e->name) + 1);
    int is_dir = (e->raw.attr & ATTR_DIRECTORY) != 0;
    rc->out->type = is_dir ? ATOS_TYPE_DIR : ATOS_TYPE_FILE;
    rc->out->size = is_dir ? 0 : e->raw.size;
    return 1;
}

static int fat_readdir(struct vnode *dir, uint64_t index, struct atos_dirent *out) {
    struct fat_node *d = (struct fat_node *)dir;
    struct readdir_ctx rc = {.want = index, .out = out};
    mutex_lock(&d->fs->lock);
    int r = dir_iterate(d->fs, d->first_cluster, readdir_visit, &rc);
    mutex_unlock(&d->fs->lock);
    if (r < 0) return r;
    return r ? 0 : -ENOENT;
}

static void fat_release(struct vnode *vn) {
    kfree(vn); /* the root holds a permanent reference, so it never gets here */
}

static const struct vnode_ops file_ops = {.read = fat_read, .release = fat_release};
static const struct vnode_ops dir_ops = {
    .lookup = fat_lookup,
    .readdir = fat_readdir,
    .release = fat_release,
};

int fat_mount(struct block_device *dev, const char *path) {
    uint8_t sector[BLOCK_SECTOR_SIZE];
    int err = block_read(dev, 0, 1, sector);
    if (err) return err;

    const struct bpb *b = (const struct bpb *)sector;
    uint32_t spc = b->sectors_per_cluster;
    if (sector[510] != 0x55 || sector[511] != 0xAA || b->bytes_per_sector != BLOCK_SECTOR_SIZE ||
        spc == 0 || (spc & (spc - 1)) != 0 || b->num_fats == 0 || b->root_entries != 0 ||
        b->fat_size16 != 0 || b->fat_size32 == 0 || b->total_sectors32 == 0) {
        return -EINVAL; /* not FAT32 (or a flavour we don't handle) */
    }

    struct fat_fs *fs = kmalloc(sizeof(*fs));
    if (!fs) return -ENOMEM;
    memset(fs, 0, sizeof(*fs));
    fs->dev = dev;
    fs->sectors_per_cluster = spc;
    fs->cluster_bytes = spc * BLOCK_SECTOR_SIZE;
    fs->fat_start = b->reserved_sectors;
    fs->fat_sectors = b->fat_size32;
    fs->num_fats = b->num_fats;
    fs->data_start = fs->fat_start + fs->num_fats * fs->fat_sectors;
    fs->cluster_count = (b->total_sectors32 - fs->data_start) / spc;
    fs->root_cluster = b->root_cluster;
    fs->cluster_buf = kmalloc(fs->cluster_bytes);
    if (!fs->cluster_buf || !valid_cluster(fs, fs->root_cluster)) {
        err = fs->cluster_buf ? -EINVAL : -ENOMEM;
        kfree(fs->cluster_buf);
        kfree(fs);
        return err;
    }

    struct fat_node *root = kmalloc(sizeof(*root));
    if (!root) {
        kfree(fs->cluster_buf);
        kfree(fs);
        return -ENOMEM;
    }
    memset(root, 0, sizeof(*root));
    root->fs = fs;
    root->first_cluster = fs->root_cluster;
    root->vn.type = ATOS_TYPE_DIR;
    root->vn.ops = &dir_ops;
    root->vn.fs_data = fs;
    root->vn.refcount = 1; /* the filesystem's own, never dropped */

    err = vfs_mount(path, &root->vn);
    if (err) return err;
    kprintf("ATOS: fat32: %s mounted at %s (%lu MiB, %u-byte clusters)\n", dev->name, path,
            (uint64_t)fs->cluster_count * fs->cluster_bytes / (1024 * 1024), fs->cluster_bytes);
    return 0;
}

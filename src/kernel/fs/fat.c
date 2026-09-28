#include "fat.h"
#include "vfs.h"
#include "../dev/block.h"
#include "../dev/rtc.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sched/sched.h"

/* FAT32 on a whole block device (no partition table). Names come from
 * VFAT long-name entries when present, otherwise from the 8.3 name with
 * its NT lowercase flags applied. Lookups are case-insensitive, as on
 * every other FAT implementation. One mutex per volume serializes all
 * access, which also protects the shared FAT-sector cache and cluster
 * buffer.
 *
 * Writing supports creating files (with long names where needed), growing
 * them, and truncating them; not yet mkdir, unlink, or rename. Each open
 * gets its own fat_node, so two opens of one file don't see each other's
 * size changes until reopened. */

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
#define FAT_EOC        0x0FFFFFFF
#define FAT_BAD        0x0FFFFFF7

#define FSINFO_LEAD_SIG   0x41615252
#define FSINFO_STRUCT_SIG 0x61417272
#define FSINFO_UNKNOWN    0xFFFFFFFF

#define LFN_CHARS_PER_ENTRY 13
#define MAX_DIR_SLOTS (1 + (ATOS_NAME_MAX + LFN_CHARS_PER_ENTRY - 1) / LFN_CHARS_PER_ENTRY)

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
    uint32_t fsinfo_sector;
    int fsinfo_invalidated;
    uint32_t next_free; /* where the next free-cluster search starts */

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

/* Byte offsets of the 13 UCS-2 characters inside a VFAT long-name entry. */
static const uint8_t lfn_char_offsets[LFN_CHARS_PER_ENTRY] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

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
                for (int i = 0; i < LFN_CHARS_PER_ENTRY; i++) {
                    uint16_t ch = (uint16_t)(b[lfn_char_offsets[i]] | (b[lfn_char_offsets[i] + 1] << 8));
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

/* --- writing --- */

static int write_cluster(struct fat_fs *fs, uint32_t cluster, const void *buf) {
    return block_write(fs->dev, cluster_lba(fs, cluster), fs->sectors_per_cluster, buf);
}

/* Sets a FAT entry in every FAT copy (write-through, so the mirrors never
 * disagree), preserving the entry's reserved top four bits. */
static int fat_set(struct fat_fs *fs, uint32_t cluster, uint32_t value) {
    uint32_t unused;
    int err = fat_get(fs, cluster, &unused); /* loads the right sector */
    if (err) return err;
    uint64_t byte = (uint64_t)cluster * 4;
    uint32_t in_sector = (uint32_t)(byte % BLOCK_SECTOR_SIZE);
    uint32_t old;
    memcpy(&old, fs->fat_cache + in_sector, 4);
    uint32_t v = (old & ~FAT_ENTRY_MASK) | (value & FAT_ENTRY_MASK);
    memcpy(fs->fat_cache + in_sector, &v, 4);

    uint32_t rel = (uint32_t)(byte / BLOCK_SECTOR_SIZE);
    for (uint32_t i = 0; i < fs->num_fats; i++) {
        err = block_write(fs->dev, fs->fat_start + i * fs->fat_sectors + rel, 1, fs->fat_cache);
        if (err) return err;
    }
    return 0;
}

/* The FSInfo sector caches the free-cluster count and a next-free hint.
 * Rather than keep them exact, mark both "unknown" (a state the spec
 * allows) before the first change to the FAT, so they can never be wrong. */
static int invalidate_fsinfo(struct fat_fs *fs) {
    if (fs->fsinfo_invalidated) return 0;
    fs->fsinfo_invalidated = 1;
    if (fs->fsinfo_sector == 0 || fs->fsinfo_sector == 0xFFFF) return 0;

    uint8_t sec[BLOCK_SECTOR_SIZE];
    int err = block_read(fs->dev, fs->fsinfo_sector, 1, sec);
    if (err) return err;
    uint32_t lead, strct;
    memcpy(&lead, sec, 4);
    memcpy(&strct, sec + 484, 4);
    if (lead != FSINFO_LEAD_SIG || strct != FSINFO_STRUCT_SIG) return 0;
    uint32_t unknown = FSINFO_UNKNOWN;
    memcpy(sec + 488, &unknown, 4); /* free cluster count */
    memcpy(sec + 492, &unknown, 4); /* next free cluster */
    return block_write(fs->dev, fs->fsinfo_sector, 1, sec);
}

/* Claims a free cluster (marked end-of-chain) and zeroes it on disk, so
 * neither file gaps nor new directory space expose stale data. */
static int alloc_cluster(struct fat_fs *fs, uint32_t *out) {
    int err = invalidate_fsinfo(fs);
    if (err) return err;
    for (uint32_t n = 0; n < fs->cluster_count; n++) {
        uint32_t c = 2 + (fs->next_free - 2 + n) % fs->cluster_count;
        uint32_t v;
        err = fat_get(fs, c, &v);
        if (err) return err;
        if (v != 0) continue;

        err = fat_set(fs, c, FAT_EOC);
        if (err) return err;
        fs->next_free = c + 1 < fs->cluster_count + 2 ? c + 1 : 2;
        memset(fs->cluster_buf, 0, fs->cluster_bytes);
        err = write_cluster(fs, c, fs->cluster_buf);
        if (err) return err;
        *out = c;
        return 0;
    }
    return -ENOSPC;
}

static int free_chain(struct fat_fs *fs, uint32_t c) {
    int err = invalidate_fsinfo(fs);
    while (!err && valid_cluster(fs, c)) {
        uint32_t next;
        err = fat_get(fs, c, &next);
        if (!err) err = fat_set(fs, c, 0);
        c = next;
    }
    return err;
}

/* Stamps a directory entry's modification (and access) time with now;
 * `created` also sets the creation time. */
static void stamp_dirent(struct raw_dirent *d, int created) {
    struct rtc_time t;
    rtc_read(&t);
    uint16_t year = t.year < 1980 ? 0 : (uint16_t)(t.year - 1980);
    uint16_t date = (uint16_t)((year << 9) | (t.month << 5) | t.day);
    uint16_t time = (uint16_t)((t.hour << 11) | (t.minute << 5) | (t.second / 2));
    d->mdate = d->adate = date;
    d->mtime = time;
    if (created) {
        d->cdate = date;
        d->ctime = time;
    }
}

/* Writes a node's size, first cluster, and modification time back into its
 * directory entry. Clobbers cluster_buf. */
static int dirent_update(struct fat_node *n) {
    struct fat_fs *fs = n->fs;
    if (!n->dirent_cluster) return 0; /* the root has no entry */
    int err = read_cluster(fs, n->dirent_cluster, fs->cluster_buf);
    if (err) return err;
    struct raw_dirent *d = (struct raw_dirent *)(fs->cluster_buf + n->dirent_offset);
    d->size = n->vn.type == ATOS_TYPE_FILE ? (uint32_t)n->vn.size : 0;
    d->cluster_hi = (uint16_t)(n->first_cluster >> 16);
    d->cluster_lo = (uint16_t)n->first_cluster;
    stamp_dirent(d, 0);
    return write_cluster(fs, n->dirent_cluster, fs->cluster_buf);
}

/* Grows a file's chain to `need` clusters. On failure the chain is cut
 * back to what it was, so the FAT never holds clusters past the size. */
static int ensure_clusters(struct fat_node *n, uint32_t need) {
    struct fat_fs *fs = n->fs;
    uint32_t have = n->first_cluster ? (uint32_t)((n->vn.size + fs->cluster_bytes - 1) / fs->cluster_bytes) : 0;
    if (n->first_cluster && have == 0) have = 1; /* empty file that still owns a cluster */
    if (have >= need) return 0;

    uint32_t old_last = 0;
    if (have) {
        int err = node_cluster_at(n, have - 1, &old_last);
        if (err) return err;
    }
    uint32_t last = old_last;
    int err = 0;
    while (have < need) {
        uint32_t c;
        err = alloc_cluster(fs, &c);
        if (err) break;
        err = last ? fat_set(fs, last, c) : 0;
        if (err) {
            fat_set(fs, c, 0);
            break;
        }
        if (!last) n->first_cluster = c;
        last = c;
        have++;
    }
    if (err) {
        if (old_last) {
            uint32_t extra;
            if (fat_get(fs, old_last, &extra) == 0) free_chain(fs, extra);
            fat_set(fs, old_last, FAT_EOC);
        } else if (n->first_cluster) {
            free_chain(fs, n->first_cluster);
            n->first_cluster = 0;
        }
        n->cursor_cluster = 0;
    }
    return err;
}

static int64_t fat_write(struct vnode *vn, uint64_t offset, const void *buf, uint64_t len) {
    struct fat_node *n = (struct fat_node *)vn;
    struct fat_fs *fs = n->fs;
    if (len == 0) return 0;
    uint64_t end = offset + len;
    if (end < offset || end > 0xFFFFFFFFULL) return -EFBIG; /* FAT sizes are 32-bit */

    mutex_lock(&fs->lock);
    uint64_t cb = fs->cluster_bytes;
    int err = ensure_clusters(n, (uint32_t)((end + cb - 1) / cb));

    /* Writing past EOF leaves a gap that must read back as zeros. New
     * clusters come zeroed; the old last cluster's tail may not be. */
    if (!err && offset > vn->size && vn->size % cb) {
        uint32_t c;
        err = node_cluster_at(n, (uint32_t)(vn->size / cb), &c);
        if (!err) err = read_cluster(fs, c, fs->cluster_buf);
        if (!err) {
            uint64_t from = vn->size % cb;
            uint64_t to = (offset / cb == vn->size / cb) ? offset % cb : cb;
            memset(fs->cluster_buf + from, 0, to - from);
            err = write_cluster(fs, c, fs->cluster_buf);
        }
    }

    uint64_t done = 0;
    while (!err && done < len) {
        uint64_t pos = offset + done;
        uint32_t c;
        err = node_cluster_at(n, (uint32_t)(pos / cb), &c);
        if (err) break;
        uint64_t in_cluster = pos % cb;
        uint64_t chunk = cb - in_cluster;
        if (chunk > len - done) chunk = len - done;
        if (chunk < cb) { /* partial cluster: read-modify-write */
            err = read_cluster(fs, c, fs->cluster_buf);
            if (err) break;
        }
        memcpy(fs->cluster_buf + in_cluster, (const uint8_t *)buf + done, chunk);
        err = write_cluster(fs, c, fs->cluster_buf);
        if (err) break;
        done += chunk;
    }

    if (offset + done > vn->size) vn->size = offset + done;
    int derr = dirent_update(n);
    mutex_unlock(&fs->lock);
    if (done) return (int64_t)done;
    return err ? err : derr;
}

static int fat_truncate(struct vnode *vn, uint64_t size) {
    struct fat_node *n = (struct fat_node *)vn;
    struct fat_fs *fs = n->fs;
    if (size > vn->size) return -EINVAL; /* only shrinking, which is all O_TRUNC needs */

    mutex_lock(&fs->lock);
    int err = 0;
    uint32_t keep = (uint32_t)((size + fs->cluster_bytes - 1) / fs->cluster_bytes);
    if (keep == 0) {
        if (n->first_cluster) err = free_chain(fs, n->first_cluster);
        n->first_cluster = 0;
    } else {
        uint32_t last, next;
        err = node_cluster_at(n, keep - 1, &last);
        if (!err) err = fat_get(fs, last, &next);
        if (!err && valid_cluster(fs, next)) err = free_chain(fs, next);
        if (!err) err = fat_set(fs, last, FAT_EOC);
    }
    n->cursor_cluster = 0; /* the cached position may be gone */
    if (!err) {
        vn->size = size;
        err = dirent_update(n);
    }
    mutex_unlock(&fs->lock);
    return err;
}

/* Characters an 8.3 name may hold (after upper-casing). */
static int short_char_ok(char c) {
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 1;
    return c && strchr("$%'-_@~`!(){}^#&", c);
}

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Fills an 11-byte 8.3 name if `name` is exactly representable as one,
 * using the NT flags for an all-lowercase base and/or extension. Mixed
 * case, long parts, or odd characters need a long-name entry instead. */
static int exact_short_name(const char *name, char out[11], uint8_t *ntres) {
    const char *dot = strrchr(name, '.');
    size_t len = strlen(name);
    size_t base_len = dot ? (size_t)(dot - name) : len;
    size_t ext_len = dot ? len - base_len - 1 : 0;
    if (base_len < 1 || base_len > 8 || ext_len > 3 || (dot && ext_len == 0)) return 0;

    memset(out, ' ', 11);
    int lower_seen[2] = {0, 0}, upper_seen[2] = {0, 0};
    for (size_t i = 0; i < len; i++) {
        if (name + i == dot) continue;
        int part = dot && name + i > dot;
        char c = name[i];
        if (c >= 'a' && c <= 'z') lower_seen[part] = 1;
        if (c >= 'A' && c <= 'Z') upper_seen[part] = 1;
        c = upper(c);
        if (!short_char_ok(c)) return 0;
        out[part ? 8 + (size_t)(name + i - dot - 1) : i] = c;
    }
    if ((lower_seen[0] && upper_seen[0]) || (lower_seen[1] && upper_seen[1])) return 0;
    *ntres = (uint8_t)((lower_seen[0] ? NTRES_LOWER_BASE : 0) | (lower_seen[1] ? NTRES_LOWER_EXT : 0));
    return 1;
}

/* The "BASIS~N.EXT" alias every long name also needs. */
static void alias_short_name(const char *name, unsigned n, char out[11]) {
    memset(out, ' ', 11);
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL;
    const char *base_end = dot ? dot : name + strlen(name);

    char tail[12];
    size_t tl = 0;
    char digits[10];
    size_t nd = 0;
    do digits[nd++] = (char)('0' + n % 10); while (n /= 10);
    tail[tl++] = '~';
    while (nd) tail[tl++] = digits[--nd];

    size_t bl = 0;
    for (const char *p = name; p < base_end && bl < 8 - tl; p++) {
        if (*p == ' ' || *p == '.') continue;
        char c = upper(*p);
        out[bl++] = short_char_ok(c) ? c : '_';
    }
    if (bl == 0) out[bl++] = '_';
    memcpy(out + bl, tail, tl);

    if (dot) {
        size_t el = 0;
        for (const char *p = dot + 1; *p && el < 3; p++) {
            if (*p == ' ' || *p == '.') continue;
            char c = upper(*p);
            out[8 + el++] = short_char_ok(c) ? c : '_';
        }
    }
}

/* Whether an 8.3 name is already taken in the directory. */
static int short_name_taken(struct fat_fs *fs, uint32_t dir_cluster, const char name[11], int *err) {
    uint32_t c = dir_cluster;
    while (valid_cluster(fs, c)) {
        *err = read_cluster(fs, c, fs->cluster_buf);
        if (*err) return 0;
        for (uint32_t off = 0; off < fs->cluster_bytes; off += sizeof(struct raw_dirent)) {
            const struct raw_dirent *d = (const struct raw_dirent *)(fs->cluster_buf + off);
            if ((uint8_t)d->name[0] == DIRENT_END) return 0;
            if ((uint8_t)d->name[0] == DIRENT_DELETED || d->attr == ATTR_LFN) continue;
            if (memcmp(d->name, name, 11) == 0) return 1;
        }
        *err = fat_get(fs, c, &c);
        if (*err) return 0;
    }
    return 0;
}

struct slot {
    uint32_t cluster, offset;
};

/* Finds `count` consecutive free directory slots, growing the directory
 * by a zeroed cluster if it runs out. Long-name runs may span clusters. */
static int find_slots(struct fat_fs *fs, uint32_t dir_cluster, unsigned count, struct slot *slots) {
    unsigned run = 0;
    uint32_t c = dir_cluster, last = 0;
    while (valid_cluster(fs, c)) {
        int err = read_cluster(fs, c, fs->cluster_buf);
        if (err) return err;
        for (uint32_t off = 0; off < fs->cluster_bytes; off += sizeof(struct raw_dirent)) {
            uint8_t first = fs->cluster_buf[off];
            if (first == DIRENT_END || first == DIRENT_DELETED) {
                slots[run].cluster = c;
                slots[run].offset = off;
                if (++run == count) return 0;
            } else {
                run = 0;
            }
        }
        last = c;
        err = fat_get(fs, c, &c);
        if (err) return err;
    }
    while (run < count) {
        uint32_t nc;
        int err = alloc_cluster(fs, &nc);
        if (!err) err = fat_set(fs, last, nc);
        if (err) return err;
        for (uint32_t off = 0; off < fs->cluster_bytes && run < count; off += sizeof(struct raw_dirent)) {
            slots[run].cluster = nc;
            slots[run].offset = off;
            run++;
        }
        last = nc;
    }
    return 0;
}

static int write_slot(struct fat_fs *fs, const struct slot *s, const void *entry) {
    int err = read_cluster(fs, s->cluster, fs->cluster_buf);
    if (err) return err;
    memcpy(fs->cluster_buf + s->offset, entry, sizeof(struct raw_dirent));
    return write_cluster(fs, s->cluster, fs->cluster_buf);
}

static int long_name_ok(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len >= ATOS_NAME_MAX) return 0;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return 0;
    if (name[len - 1] == ' ' || name[len - 1] == '.') return 0; /* Windows can't open these */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7F || strchr("\\/:*?\"<>|", (char)c)) return 0;
    }
    return 1;
}

static int fat_create(struct vnode *dir, const char *name, struct vnode **out) {
    struct fat_node *d = (struct fat_node *)dir;
    struct fat_fs *fs = d->fs;
    if (!long_name_ok(name)) return -EINVAL;

    mutex_lock(&fs->lock);
    struct lookup_ctx lc = {.name = name};
    int err = dir_iterate(fs, d->first_cluster, lookup_visit, &lc);
    if (err > 0) err = -EEXIST;
    if (err) goto out;

    struct raw_dirent entry;
    memset(&entry, 0, sizeof(entry));
    unsigned lfn_entries = 0;
    if (!exact_short_name(name, entry.name, &entry.ntres) ||
        short_name_taken(fs, d->first_cluster, entry.name, &err)) {
        if (err) goto out;
        entry.ntres = 0;
        unsigned n = 1;
        for (;; n++) {
            alias_short_name(name, n, entry.name);
            if (!short_name_taken(fs, d->first_cluster, entry.name, &err)) break;
            if (err) goto out;
            if (n == 999999) { err = -EEXIST; goto out; }
        }
        lfn_entries = (unsigned)((strlen(name) + LFN_CHARS_PER_ENTRY - 1) / LFN_CHARS_PER_ENTRY);
    }

    struct slot slots[MAX_DIR_SLOTS];
    err = find_slots(fs, d->first_cluster, lfn_entries + 1, slots);
    if (err) goto out;

    /* Long-name pieces go last-piece-first, then the 8.3 entry itself. */
    uint8_t checksum = short_name_checksum(entry.name);
    size_t len = strlen(name);
    for (unsigned i = 0; i < lfn_entries; i++) {
        unsigned piece = lfn_entries - i;
        uint8_t lfn[sizeof(struct raw_dirent)];
        memset(lfn, 0, sizeof(lfn));
        lfn[0] = (uint8_t)(piece | (i == 0 ? 0x40 : 0));
        lfn[11] = ATTR_LFN;
        lfn[13] = checksum;
        for (int k = 0; k < LFN_CHARS_PER_ENTRY; k++) {
            size_t idx = (piece - 1) * LFN_CHARS_PER_ENTRY + (size_t)k;
            uint16_t ch = idx < len ? (uint8_t)name[idx] : idx == len ? 0x0000 : 0xFFFF;
            lfn[lfn_char_offsets[k]] = (uint8_t)ch;
            lfn[lfn_char_offsets[k] + 1] = (uint8_t)(ch >> 8);
        }
        err = write_slot(fs, &slots[i], lfn);
        if (err) goto out;
    }

    entry.attr = ATTR_ARCHIVE;
    stamp_dirent(&entry, 1);
    err = write_slot(fs, &slots[lfn_entries], &entry);
    if (err) goto out;

    struct entry e = {.raw = entry, .cluster = slots[lfn_entries].cluster, .offset = slots[lfn_entries].offset};
    struct fat_node *n = node_new(fs, &e);
    if (!n) {
        err = -ENOMEM;
        goto out;
    }
    *out = &n->vn;

out:
    mutex_unlock(&fs->lock);
    return err;
}

static void fat_release(struct vnode *vn) {
    kfree(vn); /* the root holds a permanent reference, so it never gets here */
}

static const struct vnode_ops file_ops = {
    .read = fat_read,
    .write = fat_write,
    .truncate = fat_truncate,
    .release = fat_release,
};
static const struct vnode_ops dir_ops = {
    .lookup = fat_lookup,
    .readdir = fat_readdir,
    .create = fat_create,
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
    fs->fsinfo_sector = b->fsinfo_sector;
    fs->next_free = 2;
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

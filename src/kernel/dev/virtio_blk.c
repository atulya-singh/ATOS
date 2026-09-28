#include "virtio_blk.h"
#include "block.h"
#include <atos/abi.h>
#include "pci.h"
#include "pit.h"
#include "../lib/io.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/boot_info.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../sched/sched.h"

/* Legacy ("virtio 0.9.5") PCI interface: a register block in I/O space at
 * BAR0. QEMU exposes it on transitional devices (device ID 0x1001). Chosen
 * over the virtio 1.0 capability-based interface because it needs no MMIO
 * mapping or capability-list parsing. The ring format is identical, so
 * moving to modern virtio later only changes the setup code. */
#define VIRTIO_VENDOR_ID       0x1AF4
#define VIRTIO_BLK_LEGACY_ID   0x1001

#define REG_DEVICE_FEATURES 0x00
#define REG_GUEST_FEATURES  0x04
#define REG_QUEUE_PFN       0x08
#define REG_QUEUE_SIZE      0x0C
#define REG_QUEUE_SELECT    0x0E
#define REG_QUEUE_NOTIFY    0x10
#define REG_STATUS          0x12
#define REG_ISR             0x13
#define REG_BLK_CAPACITY    0x14 /* u64, in 512-byte sectors (no MSI-X) */

#define STATUS_ACKNOWLEDGE 0x01
#define STATUS_DRIVER      0x02
#define STATUS_DRIVER_OK   0x04
#define STATUS_FAILED      0x80

#define VIRTIO_BLK_F_RO (1u << 5)

#define VRING_DESC_F_NEXT  1
#define VRING_DESC_F_WRITE 2 /* device writes (vs reads) this buffer */
#define VRING_AVAIL_F_NO_INTERRUPT 1

#define VIRTIO_BLK_T_IN  0
#define VIRTIO_BLK_T_OUT 1
#define VIRTIO_BLK_S_OK  0

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[];
} __attribute__((packed));

struct blk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

/* Every transfer goes through one physically contiguous bounce buffer, so
 * callers can pass any kernel pointer (heap, stack) without caring whether
 * it's physically contiguous. 64 KiB per request is 128 sectors. */
#define BOUNCE_PAGES   16
#define BOUNCE_SECTORS (BOUNCE_PAGES * PAGE_SIZE / BLOCK_SECTOR_SIZE)
#define REQUEST_TIMEOUT_TICKS 500 /* 5 s */

struct vblk_dev {
    uint16_t io;
    uint16_t queue_size;
    struct vring_desc *desc;
    volatile struct vring_avail *avail;
    volatile struct vring_used *used;
    uint16_t last_used;

    struct blk_req_header *header; /* in the request page, device-readable */
    volatile uint8_t *status;      /* in the request page, device-writable */
    uint64_t request_phys;
    uint8_t *bounce;
    uint64_t bounce_phys;

    int read_only;
    struct mutex lock; /* one request in flight at a time */
    struct block_device blockdev;
};

/* Submits one request (header, data, status as a 3-descriptor chain) and
 * waits for the device to hand it back. Called with v->lock held. */
static int submit(struct vblk_dev *v, uint32_t type, uint64_t lba, uint32_t count) {
    v->header->type = type;
    v->header->reserved = 0;
    v->header->sector = lba;
    *v->status = 0xFF; /* anything but OK, so a no-show isn't a success */

    uint32_t bytes = count * BLOCK_SECTOR_SIZE;
    v->desc[0] = (struct vring_desc){v->request_phys, sizeof(struct blk_req_header),
                                       VRING_DESC_F_NEXT, 1};
    v->desc[1] = (struct vring_desc){v->bounce_phys, bytes,
                                       (uint16_t)(VRING_DESC_F_NEXT |
                                                  (type == VIRTIO_BLK_T_IN ? VRING_DESC_F_WRITE : 0)),
                                       2};
    v->desc[2] = (struct vring_desc){v->request_phys + sizeof(struct blk_req_header), 1,
                                       VRING_DESC_F_WRITE, 0};

    v->avail->ring[v->avail->idx % v->queue_size] = 0; /* chain head */
    __sync_synchronize(); /* descriptors visible before the index moves */
    v->avail->idx++;
    __sync_synchronize();
    outw(v->io + REG_QUEUE_NOTIFY, 0);

    /* Poll, yielding between checks: the device completes asynchronously,
     * and a blocked disk request shouldn't stall every other task. */
    uint64_t deadline = pit_get_ticks() + REQUEST_TIMEOUT_TICKS;
    while (v->used->idx == v->last_used) {
        if (pit_get_ticks() > deadline) {
            kprintf("ATOS: virtio-blk: request timed out (lba %lu)\n", lba);
            return -EIO;
        }
        sched_yield();
    }
    __sync_synchronize();
    v->last_used++;
    inb(v->io + REG_ISR); /* acknowledge, in case the device raised one anyway */

    return *v->status == VIRTIO_BLK_S_OK ? 0 : -EIO;
}

static int vblk_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf) {
    struct vblk_dev *v = dev->driver_data;
    uint8_t *out = buf;
    mutex_lock(&v->lock);
    while (count) {
        uint32_t n = count < BOUNCE_SECTORS ? count : (uint32_t)BOUNCE_SECTORS;
        int err = submit(v, VIRTIO_BLK_T_IN, lba, n);
        if (err) {
            mutex_unlock(&v->lock);
            return err;
        }
        memcpy(out, v->bounce, (size_t)n * BLOCK_SECTOR_SIZE);
        out += (size_t)n * BLOCK_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    mutex_unlock(&v->lock);
    return 0;
}

static int vblk_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf) {
    struct vblk_dev *v = dev->driver_data;
    if (v->read_only) return -EROFS;
    const uint8_t *in = buf;
    mutex_lock(&v->lock);
    while (count) {
        uint32_t n = count < BOUNCE_SECTORS ? count : (uint32_t)BOUNCE_SECTORS;
        memcpy(v->bounce, in, (size_t)n * BLOCK_SECTOR_SIZE);
        int err = submit(v, VIRTIO_BLK_T_OUT, lba, n);
        if (err) {
            mutex_unlock(&v->lock);
            return err;
        }
        in += (size_t)n * BLOCK_SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    mutex_unlock(&v->lock);
    return 0;
}

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

/* Brings up one device and registers it as vd<letter>. */
static void probe(struct pci_device *pci, char letter) {
    int is_io;
    uint64_t bar0 = pci_bar(pci, 0, &is_io);
    if (!is_io || !bar0) {
        kprintf("ATOS: virtio-blk: BAR0 isn't I/O space (legacy interface disabled?)\n");
        return;
    }
    struct vblk_dev *v = kmalloc(sizeof(*v));
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->io = (uint16_t)bar0;
    pci_enable(pci);

    /* Reset, then announce ourselves, per the legacy init sequence. */
    outb(v->io + REG_STATUS, 0);
    outb(v->io + REG_STATUS, STATUS_ACKNOWLEDGE);
    outb(v->io + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);

    uint32_t features = inl(v->io + REG_DEVICE_FEATURES);
    v->read_only = (features & VIRTIO_BLK_F_RO) != 0;
    outl(v->io + REG_GUEST_FEATURES, 0); /* nothing optional needed yet */

    outw(v->io + REG_QUEUE_SELECT, 0);
    v->queue_size = inw(v->io + REG_QUEUE_SIZE);
    if (v->queue_size == 0) goto fail;

    /* Legacy layout, fixed by the spec: descriptor table, then the avail
     * ring, then (at the next 4 KiB boundary) the used ring, all in one
     * physically contiguous, page-aligned block. */
    uint64_t n = v->queue_size;
    uint64_t used_offset = align_up(16 * n + 6 + 2 * n, PAGE_SIZE);
    uint64_t ring_bytes = used_offset + align_up(6 + 8 * n, PAGE_SIZE);
    uint64_t ring_phys = pmm_alloc_contiguous(ring_bytes / PAGE_SIZE);
    v->request_phys = pmm_alloc_page();
    v->bounce_phys = pmm_alloc_contiguous(BOUNCE_PAGES);
    if (!ring_phys || !v->request_phys || !v->bounce_phys) goto fail;

    uint8_t *ring = phys_to_virt(ring_phys);
    v->desc = (struct vring_desc *)ring;
    v->avail = (volatile struct vring_avail *)(ring + 16 * n);
    v->used = (volatile struct vring_used *)(ring + used_offset);
    v->avail->flags = VRING_AVAIL_F_NO_INTERRUPT; /* we poll */
    v->header = phys_to_virt(v->request_phys);
    v->status = (volatile uint8_t *)v->header + sizeof(struct blk_req_header);
    v->bounce = phys_to_virt(v->bounce_phys);

    outl(v->io + REG_QUEUE_PFN, (uint32_t)(ring_phys / PAGE_SIZE));
    outb(v->io + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK);

    uint64_t capacity = inl(v->io + REG_BLK_CAPACITY)
                      | ((uint64_t)inl(v->io + REG_BLK_CAPACITY + 4) << 32);

    kprintf("ATOS: virtio-blk at PCI %02x:%02x.%x, io %#x, queue size %u%s\n",
            pci->bus, pci->dev, pci->func, v->io, v->queue_size,
            v->read_only ? ", read-only" : "");

    v->blockdev.name[0] = 'v';
    v->blockdev.name[1] = 'd';
    v->blockdev.name[2] = letter;
    v->blockdev.sector_count = capacity;
    v->blockdev.driver_data = v;
    v->blockdev.read = vblk_read;
    v->blockdev.write = vblk_write;
    block_register(&v->blockdev);
    return;

fail:
    /* Ring/bounce pages already taken are leaked: this path means the
     * machine is out of memory at boot, and there's no recovering. */
    outb(v->io + REG_STATUS, STATUS_FAILED);
    kprintf("ATOS: virtio-blk: initialization failed\n");
    kfree(v);
}

void virtio_blk_init(void) {
    char letter = 'a';
    for (unsigned i = 0; i < pci_device_count() && letter <= 'z'; i++) {
        struct pci_device *pci = pci_device_at(i);
        if (pci->vendor_id == VIRTIO_VENDOR_ID && pci->device_id == VIRTIO_BLK_LEGACY_ID) {
            probe(pci, letter++);
        }
    }
}

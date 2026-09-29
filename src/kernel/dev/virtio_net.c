#include "virtio_net.h"
#include <atos/abi.h>
#include "pci.h"
#include "../lib/io.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"
#include "../mm/boot_info.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../net/net.h"

/* Legacy virtio-net (transitional device 0x1000), the same register block
 * and ring layout as virtio-blk (see virtio_blk.c). Queue 0 receives,
 * queue 1 transmits. Both are polled by the net thread: interrupts are
 * suppressed. */
#define VIRTIO_VENDOR_ID     0x1AF4
#define VIRTIO_NET_LEGACY_ID 0x1000

#define REG_DEVICE_FEATURES 0x00
#define REG_GUEST_FEATURES  0x04
#define REG_QUEUE_PFN       0x08
#define REG_QUEUE_SIZE      0x0C
#define REG_QUEUE_SELECT    0x0E
#define REG_QUEUE_NOTIFY    0x10
#define REG_STATUS          0x12
#define REG_ISR             0x13
#define REG_NET_MAC         0x14 /* 6 bytes (no MSI-X) */

#define STATUS_ACKNOWLEDGE 0x01
#define STATUS_DRIVER      0x02
#define STATUS_DRIVER_OK   0x04
#define STATUS_FAILED      0x80

#define VIRTIO_NET_F_MAC (1u << 5)

#define VRING_DESC_F_WRITE 2
#define VRING_AVAIL_F_NO_INTERRUPT 1

#define RX_QUEUE 0
#define TX_QUEUE 1
#define BUF_SIZE 2048 /* header + a full frame, two per page */
#define RX_BUFS  64
#define TX_BUFS  32

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

/* Precedes every frame in both directions. Without VIRTIO_NET_F_MRG_RXBUF
 * it is 10 bytes; all zero on transmit (no checksum offload, no GSO). */
struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

struct vqueue {
    uint16_t size;
    struct vring_desc *desc;
    volatile struct vring_avail *avail;
    volatile struct vring_used *used;
    uint16_t last_used;
    uint8_t *bufs;     /* nbufs * BUF_SIZE, physically contiguous */
    uint64_t bufs_phys;
    unsigned nbufs;
};

struct vnet_dev {
    uint16_t io;
    struct vqueue rx, tx;
    uint16_t tx_free[TX_BUFS]; /* stack of idle transmit buffers */
    unsigned tx_free_count;
    struct netif nif;
};

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int queue_setup(struct vnet_dev *v, uint16_t index, struct vqueue *q, unsigned nbufs) {
    outw(v->io + REG_QUEUE_SELECT, index);
    q->size = inw(v->io + REG_QUEUE_SIZE);
    if (q->size == 0) return -ENODEV;
    if (nbufs > q->size) nbufs = q->size;

    uint64_t n = q->size;
    uint64_t used_offset = align_up(16 * n + 6 + 2 * n, PAGE_SIZE);
    uint64_t ring_bytes = used_offset + align_up(6 + 8 * n, PAGE_SIZE);
    uint64_t ring_phys = pmm_alloc_contiguous(ring_bytes / PAGE_SIZE);
    q->bufs_phys = pmm_alloc_contiguous(align_up(nbufs * BUF_SIZE, PAGE_SIZE) / PAGE_SIZE);
    if (!ring_phys || !q->bufs_phys) return -ENOMEM;

    uint8_t *ring = phys_to_virt(ring_phys);
    q->desc = (struct vring_desc *)ring;
    q->avail = (volatile struct vring_avail *)(ring + 16 * n);
    q->used = (volatile struct vring_used *)(ring + used_offset);
    q->avail->flags = VRING_AVAIL_F_NO_INTERRUPT;
    q->bufs = phys_to_virt(q->bufs_phys);
    q->nbufs = nbufs;
    /* Descriptor i always describes buffer i. */
    for (unsigned i = 0; i < nbufs; i++) {
        q->desc[i].addr = q->bufs_phys + (uint64_t)i * BUF_SIZE;
        q->desc[i].len = BUF_SIZE;
        q->desc[i].flags = 0;
        q->desc[i].next = 0;
    }
    outl(v->io + REG_QUEUE_PFN, (uint32_t)(ring_phys / PAGE_SIZE));
    return 0;
}

static void post(struct vqueue *q, uint16_t id) {
    q->avail->ring[q->avail->idx % q->size] = id;
    __sync_synchronize(); /* the slot before the index that publishes it */
    q->avail->idx++;
}

/* Returns finished transmit buffers to the free stack. */
static void tx_reclaim(struct vnet_dev *v) {
    struct vqueue *q = &v->tx;
    while (q->used->idx != q->last_used) {
        __sync_synchronize();
        uint32_t id = q->used->ring[q->last_used % q->size].id;
        q->last_used++;
        if (id < q->nbufs) v->tx_free[v->tx_free_count++] = (uint16_t)id;
    }
}

static int vnet_transmit(struct netif *nif, const void *frame, size_t len) {
    struct vnet_dev *v = nif->driver_data;
    if (len > BUF_SIZE - sizeof(struct virtio_net_hdr)) return -EMSGSIZE;
    tx_reclaim(v);
    if (!v->tx_free_count) return -EAGAIN; /* ring full: drop, like a busy NIC */
    uint16_t id = v->tx_free[--v->tx_free_count];
    uint8_t *buf = v->tx.bufs + (size_t)id * BUF_SIZE;
    memset(buf, 0, sizeof(struct virtio_net_hdr));
    memcpy(buf + sizeof(struct virtio_net_hdr), frame, len);
    v->tx.desc[id].len = (uint32_t)(sizeof(struct virtio_net_hdr) + len);
    post(&v->tx, id);
    __sync_synchronize();
    outw(v->io + REG_QUEUE_NOTIFY, TX_QUEUE);
    return 0;
}

static int vnet_poll(struct netif *nif) {
    struct vnet_dev *v = nif->driver_data;
    struct vqueue *q = &v->rx;
    int count = 0;
    while (q->used->idx != q->last_used) {
        __sync_synchronize();
        struct vring_used_elem e = q->used->ring[q->last_used % q->size];
        q->last_used++;
        if (e.id >= q->nbufs) continue;
        if (e.len > sizeof(struct virtio_net_hdr)) {
            net_receive(nif, q->bufs + (size_t)e.id * BUF_SIZE + sizeof(struct virtio_net_hdr),
                        e.len - sizeof(struct virtio_net_hdr));
        }
        post(q, (uint16_t)e.id); /* straight back to the device */
        count++;
    }
    if (count) {
        __sync_synchronize();
        outw(v->io + REG_QUEUE_NOTIFY, RX_QUEUE);
    }
    inb(v->io + REG_ISR); /* keep the (masked) interrupt line quiet */
    return count;
}

static void probe(struct pci_device *pci) {
    int is_io;
    uint64_t bar0 = pci_bar(pci, 0, &is_io);
    if (!is_io || !bar0) {
        kprintf("ATOS: virtio-net: BAR0 isn't I/O space (legacy interface disabled?)\n");
        return;
    }
    struct vnet_dev *v = kmalloc(sizeof(*v));
    if (!v) return;
    memset(v, 0, sizeof(*v));
    v->io = (uint16_t)bar0;
    pci_enable(pci);

    outb(v->io + REG_STATUS, 0);
    outb(v->io + REG_STATUS, STATUS_ACKNOWLEDGE);
    outb(v->io + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
    uint32_t features = inl(v->io + REG_DEVICE_FEATURES);
    outl(v->io + REG_GUEST_FEATURES, features & VIRTIO_NET_F_MAC);

    if (queue_setup(v, RX_QUEUE, &v->rx, RX_BUFS) || queue_setup(v, TX_QUEUE, &v->tx, TX_BUFS)) {
        /* Pages already taken are leaked: out of memory at boot. */
        outb(v->io + REG_STATUS, STATUS_FAILED);
        kprintf("ATOS: virtio-net: initialization failed\n");
        kfree(v);
        return;
    }
    for (unsigned i = 0; i < v->rx.nbufs; i++) {
        v->rx.desc[i].flags = VRING_DESC_F_WRITE;
        post(&v->rx, (uint16_t)i);
    }
    for (unsigned i = 0; i < v->tx.nbufs; i++) v->tx_free[v->tx_free_count++] = (uint16_t)i;

    if (features & VIRTIO_NET_F_MAC) {
        for (int i = 0; i < ETH_ALEN; i++) v->nif.mac[i] = inb(v->io + REG_NET_MAC + i);
    } else { /* a locally administered address */
        static const uint8_t fallback[ETH_ALEN] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        memcpy(v->nif.mac, fallback, ETH_ALEN);
    }

    outb(v->io + REG_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_DRIVER_OK);
    outw(v->io + REG_QUEUE_NOTIFY, RX_QUEUE);

    const uint8_t *m = v->nif.mac;
    kprintf("ATOS: virtio-net at PCI %02x:%02x.%x, io %#x, MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            pci->bus, pci->dev, pci->func, v->io, m[0], m[1], m[2], m[3], m[4], m[5]);
    v->nif.transmit = vnet_transmit;
    v->nif.poll = vnet_poll;
    v->nif.driver_data = v;
    net_register(&v->nif);
}

void virtio_net_init(void) {
    for (unsigned i = 0; i < pci_device_count(); i++) {
        struct pci_device *pci = pci_device_at(i);
        if (pci->vendor_id == VIRTIO_VENDOR_ID && pci->device_id == VIRTIO_NET_LEGACY_ID) {
            probe(pci);
            return; /* one interface is all the stack handles */
        }
    }
}

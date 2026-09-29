#include "net.h"
#include "../dev/timer.h"
#include "../lib/string.h"
#include "../mm/heap.h"

#define ARP_ENTRIES      16
#define ARP_TTL_TICKS    (60 * 100) /* forget a mapping after a minute */
#define ARP_RETRY_TICKS  100
#define ARP_MAX_RETRIES  3

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY   2

struct arp_packet {
    uint16_t htype, ptype;
    uint8_t hlen, plen;
    uint16_t op;
    uint8_t sha[ETH_ALEN];
    uint32_t spa;
    uint8_t tha[ETH_ALEN];
    uint32_t tpa;
} __attribute__((packed));

/* One IPv4 -> MAC mapping. While unresolved, the most recent packet for
 * that address waits here; anything earlier is dropped, as in most
 * stacks (TCP retransmits, and ping just reports the loss). */
struct arp_entry {
    uint32_t ip;
    uint8_t mac[ETH_ALEN];
    int resolved;
    uint64_t expires;   /* resolved: when to forget; else when to retry */
    int retries;
    uint8_t *pending;
    size_t pending_len;
};

static struct arp_entry table[ARP_ENTRIES];
static const uint8_t broadcast[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void send_arp(struct netif *nif, uint16_t op, const uint8_t *tha, uint32_t tpa,
                     const uint8_t *dst_mac) {
    struct arp_packet p = {
        .htype = htons(1), .ptype = htons(ETHERTYPE_IPV4), .hlen = ETH_ALEN, .plen = 4,
        .op = htons(op), .spa = nif->ip, .tpa = tpa,
    };
    memcpy(p.sha, nif->mac, ETH_ALEN);
    memcpy(p.tha, tha, ETH_ALEN);
    eth_send(nif, dst_mac, ETHERTYPE_ARP, &p, sizeof(p));
}

static struct arp_entry *lookup(uint32_t ip) {
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (table[i].ip == ip) return &table[i];
    }
    return NULL;
}

static void entry_clear(struct arp_entry *e) {
    kfree(e->pending);
    memset(e, 0, sizeof(*e));
}

/* A free slot, or the one closest to expiring. */
static struct arp_entry *alloc_entry(uint32_t ip) {
    struct arp_entry *victim = &table[0];
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (!table[i].ip) { victim = &table[i]; break; }
        if (table[i].expires < victim->expires) victim = &table[i];
    }
    entry_clear(victim);
    victim->ip = ip;
    return victim;
}

static void learn(struct netif *nif, uint32_t ip, const uint8_t *mac, int create) {
    struct arp_entry *e = lookup(ip);
    if (!e) {
        if (!create) return;
        e = alloc_entry(ip);
    }
    memcpy(e->mac, mac, ETH_ALEN);
    e->resolved = 1;
    e->expires = timer_ticks() + ARP_TTL_TICKS;
    if (e->pending) {
        eth_send(nif, e->mac, ETHERTYPE_IPV4, e->pending, e->pending_len);
        kfree(e->pending);
        e->pending = NULL;
    }
}

void arp_input(struct netif *nif, const uint8_t *pkt, size_t len) {
    if (len < sizeof(struct arp_packet)) return;
    struct arp_packet p;
    memcpy(&p, pkt, sizeof(p));
    if (p.htype != htons(1) || p.ptype != htons(ETHERTYPE_IPV4) ||
        p.hlen != ETH_ALEN || p.plen != 4) return;

    int for_us = p.tpa == nif->ip;
    /* RFC 826: update an existing mapping from any ARP packet; add one
     * only when it's addressed to us (the sender will talk to us next). */
    if (p.spa) learn(nif, p.spa, p.sha, for_us);
    if (for_us && p.op == htons(ARP_OP_REQUEST)) {
        send_arp(nif, ARP_OP_REPLY, p.sha, p.spa, p.sha);
    }
}

int arp_send_ip(struct netif *nif, uint32_t next_hop, const void *packet, size_t len) {
    if (next_hop == 0xFFFFFFFF) return eth_send(nif, broadcast, ETHERTYPE_IPV4, packet, len);
    struct arp_entry *e = lookup(next_hop);
    if (e && e->resolved) return eth_send(nif, e->mac, ETHERTYPE_IPV4, packet, len);

    int fresh = !e;
    if (fresh) e = alloc_entry(next_hop);
    uint8_t *copy = kmalloc(len);
    if (copy) {
        memcpy(copy, packet, len);
        kfree(e->pending);
        e->pending = copy;
        e->pending_len = len;
    }
    if (fresh) {
        static const uint8_t zero[ETH_ALEN];
        send_arp(nif, ARP_OP_REQUEST, zero, next_hop, broadcast);
        e->expires = timer_ticks() + ARP_RETRY_TICKS;
    }
    return 0;
}

void arp_tick(uint64_t now) {
    struct netif *nif = net_interface();
    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *e = &table[i];
        if (!e->ip || now < e->expires) continue;
        if (e->resolved || e->retries == ARP_MAX_RETRIES) {
            entry_clear(e); /* expired, or the host never answered */
        } else {
            static const uint8_t zero[ETH_ALEN];
            send_arp(nif, ARP_OP_REQUEST, zero, e->ip, broadcast);
            e->retries++;
            e->expires = now + ARP_RETRY_TICKS;
        }
    }
}

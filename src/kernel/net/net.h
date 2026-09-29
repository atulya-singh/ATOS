#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../sched/sched.h"

/* A small IPv4 stack: Ethernet, ARP, IPv4 (no fragmentation), ICMP echo,
 * UDP, and TCP, over one network interface. All protocol state is guarded
 * by net_lock; the "net" kernel thread polls the NIC and runs the timers,
 * and socket syscalls take the lock from their own task. See docs/net.md. */

static inline uint16_t htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t ntohl(uint32_t v) { return __builtin_bswap32(v); }

#define ETH_ALEN       6
#define ETH_HLEN       14
#define ETH_MTU        1500
#define ETH_FRAME_MAX  (ETH_HLEN + ETH_MTU)
#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17
#define IP_HLEN       20 /* we never send options */

/* Formats a network-order IPv4 address for kprintf("%u.%u.%u.%u"). */
#define IP_FMT "%u.%u.%u.%u"
#define IP_ARGS(a) (unsigned)((a) & 0xFF), (unsigned)(((a) >> 8) & 0xFF), \
                   (unsigned)(((a) >> 16) & 0xFF), (unsigned)((a) >> 24)

struct netif {
    uint8_t mac[ETH_ALEN];
    uint32_t ip, netmask, gateway, dns; /* network byte order */
    /* Queues one Ethernet frame for sending; 0 or -errno. */
    int (*transmit)(struct netif *nif, const void *frame, size_t len);
    /* Hands every received frame to net_receive; returns how many. */
    int (*poll)(struct netif *nif);
    void *driver_data;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes, rx_dropped;
};

extern struct mutex net_lock;

/* Drivers: registers the interface (only the first one is used). */
void net_register(struct netif *nif);
/* The interface, or NULL if there is no NIC. */
struct netif *net_interface(void);
/* Applies the address configuration (defaults suit QEMU's user-mode
 * network; override with ip=, netmask=, gw=, dns= on the kernel command
 * line) and starts the net thread. Call after the NIC drivers. */
void net_init(void);
/* Called by a driver's poll with net_lock held. */
void net_receive(struct netif *nif, const void *frame, size_t len);

/* The Internet checksum (RFC 1071) of `len` bytes, continuing a running
 * sum; checksum_finish folds and complements it. */
uint32_t checksum_add(uint32_t sum, const void *data, size_t len);
uint16_t checksum_finish(uint32_t sum);
/* The TCP/UDP pseudo-header's contribution. */
uint32_t checksum_pseudo(uint32_t src, uint32_t dst, uint8_t proto, uint16_t len);

/* Parses dotted-quad text into a network-order address; 0 on success. */
int ip_parse(const char *s, uint32_t *out);

/* --- layers, all called with net_lock held --- */

int eth_send(struct netif *nif, const uint8_t dst[ETH_ALEN], uint16_t type,
             const void *payload, size_t len);

void arp_input(struct netif *nif, const uint8_t *pkt, size_t len);
/* Sends an IPv4 packet to the on-link `next_hop`, resolving its MAC first
 * (the packet waits in the ARP entry until the reply comes). */
int arp_send_ip(struct netif *nif, uint32_t next_hop, const void *packet, size_t len);
void arp_tick(uint64_t now);

void ipv4_input(struct netif *nif, const uint8_t *pkt, size_t len);
/* Wraps `payload` in an IPv4 header from our address to `dst` and routes it. */
int ipv4_send(uint32_t dst, uint8_t proto, const void *payload, size_t len);

void icmp_input(uint32_t src, const uint8_t *msg, size_t len);
void udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_tick(uint64_t now);

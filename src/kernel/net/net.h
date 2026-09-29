#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../sched/sched.h"
#include "inet.h"

/* A small IPv4 stack: Ethernet, ARP, IPv4 (no fragmentation), ICMP echo,
 * UDP, and TCP, over one network interface. All protocol state is guarded
 * by net_lock; the "net" kernel thread polls the NIC and runs the timers,
 * and socket syscalls take the lock from their own task. See docs/net.md. */

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
/* Sends one UDP datagram from our address, port `sport`. */
int udp_output(uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len);
void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len);
void tcp_tick(uint64_t now);

/* DHCP client (dhcp.c). dhcp_start clears the address and begins
 * discovery; `fallback` runs if the first discovery gets no answer. */
void dhcp_start(void (*fallback)(void));
void dhcp_tick(uint64_t now);
void dhcp_input(const uint8_t *msg, size_t len);
int dhcp_bound(void);

/* Logs the interface's address configuration and marks the network
 * ready. net_lock held. */
void net_print_config(void);
/* Whether the interface has its configuration (static, fallback, or a
 * DHCP lease) -- or there is no NIC, so nothing to wait for. Init is
 * started only once this holds. */
int net_ready(void);

#include "net.h"
#include "socket.h"
#include <atos/abi.h>
#include "../lib/string.h"

struct ipv4_header {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t checksum;
    uint32_t src, dst;
} __attribute__((packed));

#define IP_FLAG_DF   0x4000
#define IP_FLAG_MF   0x2000
#define IP_FRAG_MASK 0x1FFF

void ipv4_input(struct netif *nif, const uint8_t *pkt, size_t len) {
    if (len < IP_HLEN) return;
    struct ipv4_header h;
    memcpy(&h, pkt, sizeof(h));
    size_t hlen = (size_t)(h.ver_ihl & 0x0F) * 4;
    size_t total = ntohs(h.total_len);
    if ((h.ver_ihl >> 4) != 4 || hlen < IP_HLEN || total < hlen || total > len) return;
    if (checksum_finish(checksum_add(0, pkt, hlen)) != 0) return;
    /* Fragments aren't reassembled; everything we send sets DF, and QEMU's
     * gateway never needs to fragment toward us. */
    if (ntohs(h.frag) & (IP_FLAG_MF | IP_FRAG_MASK)) return;
    if (h.dst != nif->ip && h.dst != 0xFFFFFFFF) return;

    const uint8_t *payload = pkt + hlen;
    size_t plen = total - hlen;
    switch (h.proto) {
    case IP_PROTO_ICMP: icmp_input(h.src, payload, plen); break;
    case IP_PROTO_UDP:  udp_input(h.src, h.dst, payload, plen); break;
    case IP_PROTO_TCP:  tcp_input(h.src, h.dst, payload, plen); break;
    }
}

int ipv4_send(uint32_t dst, uint8_t proto, const void *payload, size_t len) {
    static uint16_t next_id;
    struct netif *nif = net_interface();
    if (!nif) return -ENETUNREACH;
    if (len > ETH_MTU - IP_HLEN) return -EMSGSIZE;

    uint8_t packet[ETH_MTU];
    struct ipv4_header h = {
        .ver_ihl = 0x45, .total_len = htons((uint16_t)(IP_HLEN + len)),
        .id = htons(next_id++), .frag = htons(IP_FLAG_DF), .ttl = 64,
        .proto = proto, .src = nif->ip, .dst = dst,
    };
    h.checksum = checksum_finish(checksum_add(0, &h, sizeof(h)));
    memcpy(packet, &h, sizeof(h));
    memcpy(packet + IP_HLEN, payload, len);

    uint32_t next_hop = dst;
    if (dst != 0xFFFFFFFF && (dst & nif->netmask) != (nif->ip & nif->netmask)) {
        next_hop = nif->gateway;
    }
    return arp_send_ip(nif, next_hop, packet, IP_HLEN + len);
}

/* --- ICMP --- */

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_echo {
    uint8_t type, code;
    uint16_t checksum;
    uint16_t id, seq;
} __attribute__((packed));

void icmp_input(uint32_t src, const uint8_t *msg, size_t len) {
    if (len < sizeof(struct icmp_echo) || len > ETH_MTU - IP_HLEN) return;
    if (checksum_finish(checksum_add(0, msg, len)) != 0) return;
    struct icmp_echo e;
    memcpy(&e, msg, sizeof(e));

    if (e.type == ICMP_ECHO_REQUEST && e.code == 0) {
        uint8_t reply[ETH_MTU - IP_HLEN];
        memcpy(reply, msg, len);
        reply[0] = ICMP_ECHO_REPLY;
        reply[2] = reply[3] = 0;
        uint16_t sum = checksum_finish(checksum_add(0, reply, len));
        memcpy(reply + 2, &sum, 2);
        ipv4_send(src, IP_PROTO_ICMP, reply, len);
    } else if (e.type == ICMP_ECHO_REPLY) {
        struct socket *s = socket_find(ATOS_SOCK_ICMP, ntohs(e.id));
        if (s) socket_queue_dgram(s, src, 0, msg, len);
    }
}

/* Sends a user-built echo request from an ICMP socket, stamping in the
 * socket's identifier and the checksum. */
int icmp_send_echo(struct socket *s, uint32_t dst, const uint8_t *msg, size_t len) {
    if (len < sizeof(struct icmp_echo) || len > ETH_MTU - IP_HLEN) return -EMSGSIZE;
    uint8_t buf[ETH_MTU - IP_HLEN];
    memcpy(buf, msg, len);
    if (buf[0] != ICMP_ECHO_REQUEST) return -EINVAL;
    uint16_t id = htons(s->lport);
    memcpy(buf + 4, &id, 2);
    buf[2] = buf[3] = 0;
    uint16_t sum = checksum_finish(checksum_add(0, buf, len));
    memcpy(buf + 2, &sum, 2);
    return ipv4_send(dst, IP_PROTO_ICMP, buf, len);
}

/* --- UDP --- */

struct udp_header {
    uint16_t sport, dport, len, checksum;
} __attribute__((packed));

void udp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len) {
    if (len < sizeof(struct udp_header)) return;
    struct udp_header h;
    memcpy(&h, seg, sizeof(h));
    size_t ulen = ntohs(h.len);
    if (ulen < sizeof(h) || ulen > len) return;
    if (h.checksum) { /* optional in IPv4 */
        uint32_t sum = checksum_pseudo(src, dst, IP_PROTO_UDP, (uint16_t)ulen);
        if (checksum_finish(checksum_add(sum, seg, ulen)) != 0) return;
    }
    struct socket *s = socket_find(ATOS_SOCK_DGRAM, ntohs(h.dport));
    if (!s) return; /* no ICMP port-unreachable: nobody here needs it */
    if (s->connected && (s->rip != src || s->rport != ntohs(h.sport))) return;
    socket_queue_dgram(s, src, ntohs(h.sport), seg + sizeof(h), ulen - sizeof(h));
}

int udp_send(struct socket *s, uint32_t dst, uint16_t dport, const void *data, size_t len) {
    if (len > ETH_MTU - IP_HLEN - sizeof(struct udp_header)) return -EMSGSIZE;
    struct netif *nif = net_interface();
    uint8_t buf[ETH_MTU - IP_HLEN];
    uint16_t ulen = (uint16_t)(sizeof(struct udp_header) + len);
    struct udp_header h = {htons(s->lport), htons(dport), htons(ulen), 0};
    memcpy(buf, &h, sizeof(h));
    memcpy(buf + sizeof(h), data, len);
    uint32_t sum = checksum_pseudo(nif->ip, dst, IP_PROTO_UDP, ulen);
    uint16_t c = checksum_finish(checksum_add(sum, buf, ulen));
    if (c == 0) c = 0xFFFF; /* 0 means "no checksum" */
    memcpy(buf + 6, &c, 2);
    return ipv4_send(dst, IP_PROTO_UDP, buf, ulen);
}

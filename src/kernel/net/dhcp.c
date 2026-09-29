#include "net.h"
#include "../dev/timer.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"

/* A DHCP client (RFC 2131), driven by the net thread: DISCOVER, take the
 * first OFFER, REQUEST it, and on ACK configure the interface. The lease
 * is renewed at T1 (half the lease) by broadcasting a REQUEST for the same
 * address; if it runs out without an ACK, discovery starts over. If
 * nobody answers the first discovery at all, the interface falls back to
 * the static configuration (see net_init). All of it runs with net_lock
 * held. */

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC 0x63825363
#define DHCP_TRIES 4 /* per phase, one second apart */

#define BOOTREQUEST 1
#define BOOTREPLY   2

#define OPT_PAD         0
#define OPT_SUBNET_MASK 1
#define OPT_ROUTER      3
#define OPT_DNS         6
#define OPT_REQUESTED   50
#define OPT_LEASE_TIME  51
#define OPT_MSG_TYPE    53
#define OPT_SERVER_ID   54
#define OPT_PARAM_LIST  55
#define OPT_END         255

#define DHCPDISCOVER 1
#define DHCPOFFER    2
#define DHCPREQUEST  3
#define DHCPACK      5
#define DHCPNAK      6

struct dhcp_packet {
    uint8_t op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;
    uint8_t options[312];
} __attribute__((packed));

enum dhcp_state { DHCP_IDLE, DHCP_SELECTING, DHCP_REQUESTING, DHCP_BOUND, DHCP_RENEWING };

static struct {
    enum dhcp_state state;
    void (*fallback)(void);
    int ever_bound;
    uint32_t xid;
    uint32_t offered, server;
    int tries;
    uint64_t next_send;  /* tick of the next retransmission */
    uint64_t renew_at, expire_at;
} dhcp;

static uint8_t *put_opt(uint8_t *p, uint8_t code, const void *data, uint8_t len) {
    *p++ = code;
    *p++ = len;
    memcpy(p, data, len);
    return p + len;
}

static void send_msg(uint8_t type) {
    struct netif *nif = net_interface();
    struct dhcp_packet pk;
    memset(&pk, 0, sizeof(pk));
    pk.op = BOOTREQUEST;
    pk.htype = 1; /* Ethernet */
    pk.hlen = ETH_ALEN;
    pk.xid = dhcp.xid;
    /* Ask for broadcast replies: with no address yet we can't take
     * unicast ones reliably. */
    pk.flags = htons(0x8000);
    memcpy(pk.chaddr, nif->mac, ETH_ALEN);
    pk.magic = htonl(DHCP_MAGIC);

    uint8_t *p = pk.options;
    p = put_opt(p, OPT_MSG_TYPE, &type, 1);
    if (type == DHCPREQUEST) {
        if (dhcp.state == DHCP_RENEWING) {
            pk.ciaddr = nif->ip; /* renewing: the address is ours already */
        } else {
            p = put_opt(p, OPT_REQUESTED, &dhcp.offered, 4);
            p = put_opt(p, OPT_SERVER_ID, &dhcp.server, 4);
        }
    }
    static const uint8_t params[] = {OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS, OPT_LEASE_TIME};
    p = put_opt(p, OPT_PARAM_LIST, params, sizeof(params));
    *p++ = OPT_END;
    /* BOOTP relays expect at least the classic 300-byte message. */
    size_t len = (size_t)(p - (uint8_t *)&pk);
    if (len < 300) len = 300;
    udp_output(DHCP_CLIENT_PORT, 0xFFFFFFFF, DHCP_SERVER_PORT, &pk, len);
}

static void begin(enum dhcp_state state, uint64_t now) {
    dhcp.state = state;
    dhcp.tries = 1;
    dhcp.next_send = now + TIMER_HZ;
    send_msg(state == DHCP_SELECTING ? DHCPDISCOVER : DHCPREQUEST);
}

void dhcp_start(void (*fallback)(void)) {
    struct netif *nif = net_interface();
    dhcp.fallback = fallback;
    /* Any value works; mixing in the MAC and the clock keeps two ATOS
     * guests on one network from sharing transaction ids. */
    dhcp.xid = (uint32_t)timer_ticks() * 2654435761u ^ ((uint32_t)nif->mac[4] << 8 | nif->mac[5]);
    nif->ip = nif->netmask = nif->gateway = nif->dns = 0;
    kprintf("ATOS: net: dhcp: discovering\n");
    begin(DHCP_SELECTING, timer_ticks());
}

int dhcp_active(void) {
    return dhcp.state == DHCP_SELECTING || dhcp.state == DHCP_REQUESTING;
}

int dhcp_bound(void) {
    return dhcp.state == DHCP_BOUND || dhcp.state == DHCP_RENEWING;
}

void dhcp_tick(uint64_t now) {
    struct netif *nif = net_interface();
    switch (dhcp.state) {
    case DHCP_IDLE:
        break;
    case DHCP_SELECTING:
    case DHCP_REQUESTING:
        if (now < dhcp.next_send) break;
        if (dhcp.tries >= DHCP_TRIES) {
            if (!dhcp.ever_bound) {
                kprintf("ATOS: net: dhcp: no answer, using the static configuration\n");
                dhcp.state = DHCP_IDLE;
                dhcp.fallback();
            } else {
                begin(DHCP_SELECTING, now); /* keep trying: we had a network once */
            }
            break;
        }
        dhcp.tries++;
        dhcp.next_send = now + TIMER_HZ;
        send_msg(dhcp.state == DHCP_SELECTING ? DHCPDISCOVER : DHCPREQUEST);
        break;
    case DHCP_BOUND:
        if (now >= dhcp.renew_at) begin(DHCP_RENEWING, now);
        break;
    case DHCP_RENEWING:
        if (now >= dhcp.expire_at) {
            kprintf("ATOS: net: dhcp: lease on " IP_FMT " expired\n", IP_ARGS(nif->ip));
            dhcp_start(dhcp.fallback);
        } else if (now >= dhcp.next_send) {
            /* Retry every few seconds until the lease runs out. */
            dhcp.next_send = now + 4 * TIMER_HZ;
            send_msg(DHCPREQUEST);
        }
        break;
    }
}

/* Reads a 4-byte option value (addresses stay in network order). */
static int opt_u32(const uint8_t *opt, uint8_t len, uint32_t *out) {
    if (len < 4) return 0;
    memcpy(out, opt, 4);
    return 1;
}

void dhcp_input(const uint8_t *msg, size_t len) {
    struct netif *nif = net_interface();
    if (dhcp.state == DHCP_IDLE || dhcp.state == DHCP_BOUND) return;
    const size_t fixed = offsetof(struct dhcp_packet, options);
    if (len < fixed) return;
    struct dhcp_packet pk;
    memset(&pk, 0, sizeof(pk));
    memcpy(&pk, msg, len < sizeof(pk) ? len : sizeof(pk));
    if (pk.op != BOOTREPLY || pk.xid != dhcp.xid || ntohl(pk.magic) != DHCP_MAGIC ||
        memcmp(pk.chaddr, nif->mac, ETH_ALEN) != 0) {
        return;
    }

    uint8_t type = 0;
    uint32_t server = 0, mask = 0, router = 0, dns = 0, lease = 0;
    const uint8_t *p = pk.options, *end = (const uint8_t *)&pk + (len < sizeof(pk) ? len : sizeof(pk));
    while (p < end && *p != OPT_END) {
        if (*p == OPT_PAD) {
            p++;
            continue;
        }
        if (p + 2 > end || p + 2 + p[1] > end) break;
        uint8_t code = p[0], olen = p[1];
        const uint8_t *v = p + 2;
        switch (code) {
        case OPT_MSG_TYPE:    if (olen >= 1) type = v[0]; break;
        case OPT_SERVER_ID:   opt_u32(v, olen, &server); break;
        case OPT_SUBNET_MASK: opt_u32(v, olen, &mask); break;
        case OPT_ROUTER:      opt_u32(v, olen, &router); break; /* the first one */
        case OPT_DNS:         opt_u32(v, olen, &dns); break;
        case OPT_LEASE_TIME:  if (opt_u32(v, olen, &lease)) lease = ntohl(lease); break;
        }
        p += 2 + olen;
    }

    uint64_t now = timer_ticks();
    if (type == DHCPOFFER && dhcp.state == DHCP_SELECTING && pk.yiaddr && server) {
        dhcp.offered = pk.yiaddr;
        dhcp.server = server;
        begin(DHCP_REQUESTING, now);
    } else if (type == DHCPACK && (dhcp.state == DHCP_REQUESTING || dhcp.state == DHCP_RENEWING)) {
        int renewed = dhcp.state == DHCP_RENEWING && pk.yiaddr == nif->ip;
        nif->ip = pk.yiaddr;
        /* No mask option: assume the classic /24 rather than /0. */
        nif->netmask = mask ? mask : htonl(0xFFFFFF00);
        nif->gateway = router;
        nif->dns = dns ? dns : router;
        if (lease == 0) lease = 3600;
        if (lease == 0xFFFFFFFF) lease = 0x7FFFFFFF; /* "infinite" */
        dhcp.renew_at = now + (uint64_t)lease / 2 * TIMER_HZ;
        dhcp.expire_at = now + (uint64_t)lease * TIMER_HZ;
        dhcp.state = DHCP_BOUND;
        dhcp.ever_bound = 1;
        if (renewed) return;
        kprintf("ATOS: net: dhcp: leased " IP_FMT " from " IP_FMT " for %us\n", IP_ARGS(nif->ip),
                IP_ARGS(server ? server : dhcp.server), lease);
        net_print_config();
    } else if (type == DHCPNAK && dhcp.state != DHCP_SELECTING) {
        kprintf("ATOS: net: dhcp: request refused, starting over\n");
        dhcp_start(dhcp.fallback);
    }
}

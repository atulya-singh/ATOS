#include "net.h"
#include "socket.h"
#include "../dev/timer.h"
#include "../lib/cmdline.h"
#include "../lib/kprintf.h"
#include "../lib/string.h"

struct mutex net_lock;
static struct netif *iface;

void net_register(struct netif *nif) {
    if (!iface) iface = nif;
}

struct netif *net_interface(void) { return iface; }

int eth_send(struct netif *nif, const uint8_t dst[ETH_ALEN], uint16_t type,
             const void *payload, size_t len) {
    if (len > ETH_MTU) return -EMSGSIZE;
    uint8_t frame[ETH_FRAME_MAX];
    memcpy(frame, dst, ETH_ALEN);
    memcpy(frame + 6, nif->mac, ETH_ALEN);
    frame[12] = (uint8_t)(type >> 8);
    frame[13] = (uint8_t)type;
    memcpy(frame + ETH_HLEN, payload, len);
    size_t total = ETH_HLEN + len;
    if (total < 60) { /* pad to Ethernet's minimum frame */
        memset(frame + total, 0, 60 - total);
        total = 60;
    }
    int err = nif->transmit(nif, frame, total);
    if (err) return err;
    nif->tx_packets++;
    nif->tx_bytes += total;
    return 0;
}

void net_receive(struct netif *nif, const void *frame, size_t len) {
    const uint8_t *f = frame;
    if (len < ETH_HLEN) {
        nif->rx_dropped++;
        return;
    }
    nif->rx_packets++;
    nif->rx_bytes += len;
    uint16_t type = (uint16_t)(f[12] << 8 | f[13]);
    if (type == ETHERTYPE_ARP) arp_input(nif, f + ETH_HLEN, len - ETH_HLEN);
    else if (type == ETHERTYPE_IPV4) ipv4_input(nif, f + ETH_HLEN, len - ETH_HLEN);
}

/* Polls the NIC and runs the protocol timers. There is no NIC interrupt
 * (legacy INTx routing on q35 needs the ACPI _PRT, i.e. an AML
 * interpreter), so: after traffic, poll every scheduling round for a few
 * ticks to keep latency low during an exchange; when quiet, once a tick. */
static void net_thread(void *arg) {
    (void)arg;
    uint64_t last_tick = 0, busy_until = 0;
    for (;;) {
        mutex_lock(&net_lock);
        int n = iface->poll(iface);
        uint64_t now = timer_ticks();
        if (now != last_tick) {
            last_tick = now;
            arp_tick(now);
            tcp_tick(now);
            socket_tick(now);
        }
        mutex_unlock(&net_lock);
        if (n) busy_until = now + 3;
        if (now < busy_until) sched_yield();
        else task_sleep(1);
    }
}

static void config_addr(const char *key, uint32_t *field, const char *fallback) {
    char buf[20];
    const char *text = cmdline_get(key, buf, sizeof(buf)) ? buf : fallback;
    if (ip_parse(text, field)) {
        kprintf("ATOS: net: bad %s=%s, using %s\n", key, text, fallback);
        ip_parse(fallback, field);
    }
}

void net_init(void) {
    if (!iface) {
        kprintf("ATOS: net: no network interface\n");
        return;
    }
    /* QEMU user-mode networking's fixed layout: we are 10.0.2.15, the
     * host/gateway is 10.0.2.2, its DNS forwarder 10.0.2.3. */
    config_addr("ip", &iface->ip, "10.0.2.15");
    config_addr("netmask", &iface->netmask, "255.255.255.0");
    config_addr("gw", &iface->gateway, "10.0.2.2");
    config_addr("dns", &iface->dns, "10.0.2.3");
    kprintf("ATOS: net: " IP_FMT "/" IP_FMT " gateway " IP_FMT " dns " IP_FMT "\n",
            IP_ARGS(iface->ip), IP_ARGS(iface->netmask), IP_ARGS(iface->gateway),
            IP_ARGS(iface->dns));
    if (!task_create_kernel("net", net_thread, NULL)) {
        kprintf("ATOS: net: could not start the net thread\n");
    }
}

#include <arpa/inet.h>
#include <atos.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static const char *ip(uint32_t a) {
    struct in_addr in = {a};
    return inet_ntoa(in);
}

/* Shows the network interface's addresses and traffic counters. */
int main(void) {
    struct atos_netinfo n;
    if (netinfo(&n) < 0) {
        dprintf(STDERR_FILENO, "ifconfig: %s\n", strerror(errno));
        return 1;
    }
    /* inet_ntoa reuses one buffer, so one address per printf. */
    printf("eth0: inet %s", ip(n.addr));
    printf(" netmask %s", ip(n.netmask));
    printf(" gateway %s", ip(n.gateway));
    printf(" dns %s\n", ip(n.dns));
    printf("      ether %02x:%02x:%02x:%02x:%02x:%02x\n",
           n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    printf("      rx %lu packets (%lu bytes, %lu dropped), tx %lu packets (%lu bytes)\n",
           (unsigned long)n.rx_packets, (unsigned long)n.rx_bytes, (unsigned long)n.rx_dropped,
           (unsigned long)n.tx_packets, (unsigned long)n.tx_bytes);
    return 0;
}

#include <arpa/inet.h>
#include <atos.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* nslookup name [server[:port]]: one DNS A query, by default to the
 * interface's DNS server. */
int main(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        dprintf(STDERR_FILENO, "usage: nslookup name [server[:port]]\n");
        return 2;
    }
    struct atos_netinfo info;
    if (netinfo(&info) < 0) {
        dprintf(STDERR_FILENO, "nslookup: %s\n", strerror(errno));
        return 2;
    }
    uint32_t server = info.dns;
    int port = 53;
    if (argc == 3) {
        char buf[32];
        strncpy(buf, argv[2], sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';
        char *colon = strchr(buf, ':');
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        struct in_addr a;
        if (!inet_aton(buf, &a) || port <= 0 || port > 65535) {
            dprintf(STDERR_FILENO, "nslookup: bad server '%s'\n", argv[2]);
            return 2;
        }
        server = a.s_addr;
    }
    struct in_addr s = {server};
    printf("Server:  %s#%d\n", inet_ntoa(s), port);

    uint32_t addr;
    if (dns_resolve(argv[1], server, (uint16_t)port, 2000, &addr) < 0) {
        printf("** can't find %s: %s\n", argv[1], errno == ENOENT ? "no such name (NXDOMAIN)" : strerror(errno));
        return 1;
    }
    struct in_addr a = {addr};
    printf("Name:    %s\nAddress: %s\n", argv[1], inet_ntoa(a));
    return 0;
}

#include <arpa/inet.h>
#include <atos.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PAYLOAD 56

struct echo {
    uint8_t type, code;
    uint16_t checksum, id, seq;
    uint8_t data[PAYLOAD];
} __attribute__((packed));

/* ping [-c count] host: ICMP echo through a ping socket, one a second. */
int main(int argc, char **argv) {
    int count = 4, i = 1;
    if (argc > 2 && strcmp(argv[1], "-c") == 0) {
        count = atoi(argv[2]);
        i = 3;
    }
    if (i != argc - 1 || count <= 0) {
        dprintf(STDERR_FILENO, "usage: ping [-c count] host\n");
        return 2;
    }
    struct hostent *h = gethostbyname(argv[i]);
    if (!h) {
        dprintf(STDERR_FILENO, "ping: %s: unknown host\n", argv[i]);
        return 2;
    }
    struct sockaddr_in to = {.sin_family = AF_INET};
    memcpy(&to.sin_addr, h->h_addr, 4);
    char target[16];
    strcpy(target, inet_ntoa(to.sin_addr));

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (fd < 0) {
        dprintf(STDERR_FILENO, "ping: socket: %s\n", strerror(errno));
        return 2;
    }
    printf("PING %s (%s): %d data bytes\n", argv[i], target, PAYLOAD);

    int received = 0;
    for (int seq = 1; seq <= count; seq++) {
        struct echo req = {.type = 8, .seq = htons((uint16_t)seq)};
        for (int b = 0; b < PAYLOAD; b++) req.data[b] = (uint8_t)b;
        unsigned long sent_at = uptime_ms();
        if (sendto(fd, &req, sizeof(req), 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            printf("ping: sendto: %s\n", strerror(errno));
        } else {
            /* Wait out the rest of this second for the matching reply. */
            for (;;) {
                long left = 1000 - (long)(uptime_ms() - sent_at);
                if (left <= 0) {
                    printf("request timeout for icmp_seq=%d\n", seq);
                    break;
                }
                struct timeval tv = {left / 1000, (left % 1000) * 1000};
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                struct echo rep;
                struct sockaddr_in from;
                socklen_t flen = sizeof(from);
                long n = recvfrom(fd, &rep, sizeof(rep), 0, (struct sockaddr *)&from, &flen);
                if (n < 0) {
                    if (errno != ETIMEDOUT) printf("ping: recvfrom: %s\n", strerror(errno));
                    printf("request timeout for icmp_seq=%d\n", seq);
                    break;
                }
                if (n < 8 || ntohs(rep.seq) != seq) continue; /* a late reply */
                printf("%ld bytes from %s: icmp_seq=%d time=%lu ms\n",
                       n, inet_ntoa(from.sin_addr), seq, uptime_ms() - sent_at);
                received++;
                break;
            }
        }
        unsigned long spent = uptime_ms() - sent_at;
        if (seq < count && spent < 1000) msleep(1000 - spent);
    }
    close(fd);
    printf("--- %s ping statistics ---\n", argv[i]);
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           count, received, (count - received) * 100 / count);
    return received ? 0 : 1;
}

#pragma once
#include <stddef.h>
#include <stdint.h>
#include <atos/abi.h>

/* BSD sockets over ATOS's socket syscalls (IPv4 only). As on Linux,
 * socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP) gives an unprivileged ping
 * socket: send ICMP echo requests, receive the matching replies. */

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

#define AF_INET ATOS_AF_INET
#define PF_INET AF_INET

#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define SOL_SOCKET  1
#define SO_RCVTIMEO 20

struct sockaddr {
    sa_family_t sa_family;
    char sa_data[14];
};

struct timeval {
    long tv_sec;
    long tv_usec;
};

int socket(int domain, int type, int protocol);
int bind(int fd, const struct sockaddr *addr, socklen_t len);
int connect(int fd, const struct sockaddr *addr, socklen_t len);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *addr, socklen_t *len);
long send(int fd, const void *buf, size_t len, int flags);
long recv(int fd, void *buf, size_t len, int flags);
long sendto(int fd, const void *buf, size_t len, int flags,
            const struct sockaddr *dest, socklen_t dest_len);
long recvfrom(int fd, void *buf, size_t len, int flags,
              struct sockaddr *src, socklen_t *src_len);
/* Only SOL_SOCKET/SO_RCVTIMEO (a struct timeval) is supported. */
int setsockopt(int fd, int level, int name, const void *value, socklen_t len);

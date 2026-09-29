#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../fs/vfs.h"
#include "../sched/sched.h"

/* A received UDP datagram or ICMP message, queued on its socket. */
struct dgram {
    struct dgram *next;
    uint32_t src;   /* network order */
    uint16_t sport; /* host order */
    uint16_t len;
    uint8_t data[];
};

struct tcp_conn;

/* A socket is a vnode (so read, write, close, dup2, and fork work on it
 * like any file) whose fs_data points here. Fields below are guarded by
 * net_lock. */
struct socket {
    int type;                /* ATOS_SOCK_* */
    uint32_t lip, rip;       /* network order; lip 0 = any */
    uint16_t lport, rport;   /* host order; for ICMP, lport is the echo id */
    int connected;           /* rip/rport are the default destination */
    uint64_t timeout_ms;     /* ATOS_SO_RCVTIMEO */

    struct wait_queue wq;
    volatile uint64_t events; /* bumped by socket_notify */
    uint64_t deadline;        /* tick a blocked call gives up at; 0 = none */

    struct dgram *rx_head, *rx_tail;
    unsigned rx_count;

    struct tcp_conn *conn;   /* stream sockets, once connecting or listening */
    struct socket *next;     /* every open socket */
    struct vnode vn;
};

/* Wakes whoever is blocked on `s`. net_lock held. */
void socket_notify(struct socket *s);

/* Blocks until socket_notify(s) or the call's deadline (see
 * socket_deadline_start). Called and returns with net_lock held; returns
 * -ETIMEDOUT once the deadline has passed, else 0 -- the caller rechecks
 * its condition either way. */
int socket_wait(struct socket *s);
/* Arms/disarms the ATOS_SO_RCVTIMEO deadline around one blocking call. */
void socket_deadline_start(struct socket *s);
void socket_deadline_end(struct socket *s);

/* Creates a socket vnode and its struct socket, not yet on any fd.
 * net_lock held. */
struct socket *socket_new(int type);

/* The socket bound to (type, lport), or NULL. */
struct socket *socket_find(int type, uint16_t lport);
/* A free ephemeral port for `type`. */
uint16_t socket_ephemeral_port(int type);
/* Queues a datagram on `s` (dropped if the queue is full). */
void socket_queue_dgram(struct socket *s, uint32_t src, uint16_t sport, const void *data, size_t len);

/* Wakes sockets whose deadline has passed. net_lock held. */
void socket_tick(uint64_t now);

/* The syscalls; user pointers are validated here. */
int64_t sys_socket(int type);
int64_t sys_bind(int fd, uint64_t uaddr);
int64_t sys_connect(int fd, uint64_t uaddr);
int64_t sys_listen(int fd, int backlog);
int64_t sys_accept(int fd, uint64_t uaddr);
int64_t sys_sendto(int fd, uint64_t ubuf, uint64_t len, uint64_t uaddr);
int64_t sys_recvfrom(int fd, uint64_t ubuf, uint64_t len, uint64_t uaddr);
int64_t sys_sockopt(int fd, int opt, uint64_t value);
int64_t sys_netinfo(uint64_t uinfo);

/* --- TCP's side of the socket interface (tcp.c); net_lock held --- */
int tcp_sock_connect(struct socket *s, uint32_t dst, uint16_t dport);
int tcp_sock_listen(struct socket *s, int backlog);
/* Returns a new, connected socket (with a vnode reference) or NULL + *err. */
struct socket *tcp_sock_accept(struct socket *s, int *err);
int64_t tcp_sock_send(struct socket *s, const void *buf, uint64_t len);
int64_t tcp_sock_recv(struct socket *s, void *buf, uint64_t len);
void tcp_sock_close(struct socket *s);

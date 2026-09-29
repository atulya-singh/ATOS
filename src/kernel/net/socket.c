#include "socket.h"
#include "net.h"
#include <atos/abi.h>
#include "../dev/timer.h"
#include "../lib/string.h"
#include "../mm/heap.h"
#include "../sys/uaccess.h"

#define DGRAM_QUEUE_MAX 32
#define EPHEMERAL_FIRST 49152
#define IO_MAX          (64 * 1024) /* per call, as for read/write */

static struct socket *sockets;

int icmp_send_echo(struct socket *s, uint32_t dst, const uint8_t *msg, size_t len);
int udp_send(struct socket *s, uint32_t dst, uint16_t dport, const void *data, size_t len);

void socket_notify(struct socket *s) {
    s->events++;
    wait_queue_wake_all(&s->wq);
}

int socket_wait(struct socket *s) {
    uint64_t deadline = s->deadline;
    if (deadline && timer_ticks() >= deadline) return -ETIMEDOUT;
    uint64_t seen = s->events;
    mutex_unlock(&net_lock);
    wait_event(&s->wq, s->events != seen || (deadline && timer_ticks() >= deadline));
    mutex_lock(&net_lock);
    return 0;
}

void socket_deadline_start(struct socket *s) {
    s->deadline = s->timeout_ms
        ? timer_ticks() + (s->timeout_ms * TIMER_HZ + 999) / 1000 : 0;
}

void socket_deadline_end(struct socket *s) { s->deadline = 0; }

void socket_tick(uint64_t now) {
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->deadline && now >= s->deadline) wait_queue_wake_all(&s->wq);
    }
}

struct socket *socket_find(int type, uint16_t lport) {
    for (struct socket *s = sockets; s; s = s->next) {
        if (s->type == type && s->lport == lport) return s;
    }
    return NULL;
}

uint16_t socket_ephemeral_port(int type) {
    static uint16_t next = EPHEMERAL_FIRST;
    for (;;) {
        uint16_t p = next;
        next = next == 0xFFFF ? EPHEMERAL_FIRST : next + 1;
        if (!socket_find(type, p)) return p;
    }
}

void socket_queue_dgram(struct socket *s, uint32_t src, uint16_t sport, const void *data, size_t len) {
    if (s->rx_count == DGRAM_QUEUE_MAX) return;
    struct dgram *d = kmalloc(sizeof(*d) + len);
    if (!d) return;
    d->next = NULL;
    d->src = src;
    d->sport = sport;
    d->len = (uint16_t)len;
    memcpy(d->data, data, len);
    if (s->rx_tail) s->rx_tail->next = d;
    else s->rx_head = d;
    s->rx_tail = d;
    s->rx_count++;
    socket_notify(s);
}

/* --- the vnode side: read/write/close on a socket fd --- */

static int64_t dgram_send(struct socket *s, const void *buf, uint64_t len, uint32_t dst, uint16_t dport);
static int64_t dgram_recv(struct socket *s, void *buf, uint64_t len, uint32_t *src, uint16_t *sport);

static int64_t sock_read(struct vnode *vn, uint64_t offset, void *buf, uint64_t len) {
    (void)offset;
    struct socket *s = vn->fs_data;
    mutex_lock(&net_lock);
    int64_t n = s->type == ATOS_SOCK_STREAM ? tcp_sock_recv(s, buf, len)
                                            : dgram_recv(s, buf, len, NULL, NULL);
    mutex_unlock(&net_lock);
    return n;
}

static int64_t sock_write(struct vnode *vn, uint64_t offset, const void *buf, uint64_t len) {
    (void)offset;
    struct socket *s = vn->fs_data;
    mutex_lock(&net_lock);
    int64_t n;
    if (s->type == ATOS_SOCK_STREAM) n = tcp_sock_send(s, buf, len);
    else if (!s->connected) n = -EDESTADDRREQ;
    else n = dgram_send(s, buf, len, s->rip, s->rport);
    mutex_unlock(&net_lock);
    return n;
}

static void sock_release(struct vnode *vn) {
    struct socket *s = vn->fs_data;
    mutex_lock(&net_lock);
    for (struct socket **pp = &sockets; *pp; pp = &(*pp)->next) {
        if (*pp == s) { *pp = s->next; break; }
    }
    if (s->type == ATOS_SOCK_STREAM) tcp_sock_close(s);
    while (s->rx_head) {
        struct dgram *d = s->rx_head;
        s->rx_head = d->next;
        kfree(d);
    }
    mutex_unlock(&net_lock);
    kfree(s);
}

static const struct vnode_ops sock_ops = {
    .read = sock_read,
    .write = sock_write,
    .release = sock_release,
};

struct socket *socket_new(int type) {
    struct socket *s = kmalloc(sizeof(*s));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->type = type;
    s->vn.type = ATOS_TYPE_SOCKET;
    s->vn.ops = &sock_ops;
    s->vn.fs_data = s;
    s->vn.refcount = 1;
    s->next = sockets;
    sockets = s;
    return s;
}

/* --- datagram (UDP, ICMP) I/O; net_lock held --- */

static int64_t dgram_send(struct socket *s, const void *buf, uint64_t len, uint32_t dst, uint16_t dport) {
    if (!s->lport) s->lport = socket_ephemeral_port(s->type);
    int err = s->type == ATOS_SOCK_ICMP ? icmp_send_echo(s, dst, buf, len)
                                        : udp_send(s, dst, dport, buf, len);
    return err ? err : (int64_t)len;
}

static int64_t dgram_recv(struct socket *s, void *buf, uint64_t len, uint32_t *src, uint16_t *sport) {
    socket_deadline_start(s);
    while (!s->rx_head) {
        int err = socket_wait(s);
        if (err) {
            socket_deadline_end(s);
            return err;
        }
    }
    socket_deadline_end(s);
    struct dgram *d = s->rx_head;
    s->rx_head = d->next;
    if (!s->rx_head) s->rx_tail = NULL;
    s->rx_count--;
    uint64_t n = d->len < len ? d->len : len; /* the rest of the datagram is lost, as in BSD */
    memcpy(buf, d->data, n);
    if (src) *src = d->src;
    if (sport) *sport = d->sport;
    kfree(d);
    return (int64_t)n;
}

/* --- syscalls --- */

static struct socket *fd_socket(int fd) {
    struct file *f = fd_get(sched_current(), fd);
    if (!f || f->vn->type != ATOS_TYPE_SOCKET) return NULL;
    return f->vn->fs_data;
}

static int get_addr(uint64_t uaddr, struct atos_sockaddr_in *out) {
    if (copy_from_user(out, uaddr, sizeof(*out))) return -EFAULT;
    return out->family == ATOS_AF_INET ? 0 : -EINVAL;
}

static int install(struct socket *s) {
    struct file *f = kmalloc(sizeof(*f));
    if (!f) {
        vnode_release(&s->vn);
        return -ENOMEM;
    }
    f->vn = &s->vn;
    f->offset = 0;
    f->flags = O_RDWR;
    f->refcount = 1;
    int fd = fd_install(sched_current(), f);
    if (fd < 0) file_close(f);
    return fd;
}

int64_t sys_socket(int type) {
    if (type != ATOS_SOCK_STREAM && type != ATOS_SOCK_DGRAM && type != ATOS_SOCK_ICMP) return -EPROTOTYPE;
    if (!net_interface()) return -ENODEV;
    mutex_lock(&net_lock);
    struct socket *s = socket_new(type);
    mutex_unlock(&net_lock);
    return s ? install(s) : -ENOMEM;
}

int64_t sys_bind(int fd, uint64_t uaddr) {
    struct socket *s = fd_socket(fd);
    if (!s) return -ENOTSOCK;
    struct atos_sockaddr_in a;
    int err = get_addr(uaddr, &a);
    if (err) return err;
    mutex_lock(&net_lock);
    uint16_t port = ntohs(a.port);
    if (s->lport) err = -EINVAL;
    else if (port && socket_find(s->type, port)) err = -EADDRINUSE;
    else s->lport = port ? port : socket_ephemeral_port(s->type);
    mutex_unlock(&net_lock);
    return err;
}

int64_t sys_connect(int fd, uint64_t uaddr) {
    struct socket *s = fd_socket(fd);
    if (!s) return -ENOTSOCK;
    struct atos_sockaddr_in a;
    int err = get_addr(uaddr, &a);
    if (err) return err;
    mutex_lock(&net_lock);
    if (s->type == ATOS_SOCK_STREAM) {
        err = tcp_sock_connect(s, a.addr, ntohs(a.port));
    } else {
        s->rip = a.addr;
        s->rport = ntohs(a.port);
        s->connected = 1;
        if (!s->lport) s->lport = socket_ephemeral_port(s->type);
    }
    mutex_unlock(&net_lock);
    return err;
}

int64_t sys_listen(int fd, int backlog) {
    struct socket *s = fd_socket(fd);
    if (!s) return -ENOTSOCK;
    if (s->type != ATOS_SOCK_STREAM) return -EOPNOTSUPP;
    mutex_lock(&net_lock);
    int err = tcp_sock_listen(s, backlog);
    mutex_unlock(&net_lock);
    return err;
}

int64_t sys_accept(int fd, uint64_t uaddr) {
    struct socket *s = fd_socket(fd);
    if (!s) return -ENOTSOCK;
    if (s->type != ATOS_SOCK_STREAM) return -EOPNOTSUPP;
    if (uaddr && !user_range_ok(uaddr, sizeof(struct atos_sockaddr_in), 1)) return -EFAULT;
    int err;
    mutex_lock(&net_lock);
    struct socket *ns = tcp_sock_accept(s, &err);
    struct atos_sockaddr_in a = {.family = ATOS_AF_INET};
    if (ns) {
        a.addr = ns->rip;
        a.port = htons(ns->rport);
    }
    mutex_unlock(&net_lock);
    if (!ns) return err;
    if (uaddr) copy_to_user(uaddr, &a, sizeof(a));
    return install(ns);
}

int64_t sys_sendto(int fd, uint64_t ubuf, uint64_t len, uint64_t uaddr) {
    struct socket *s = fd_socket(fd);
    if (!s) return -ENOTSOCK;
    if (len > IO_MAX) len = IO_MAX;
    if (!user_range_ok(ubuf, len, 0)) return -EFAULT;
    if (s->type == ATOS_SOCK_STREAM || !uaddr) return vfs_write(fd_get(sched_current(), fd), (const void *)ubuf, len);
    struct atos_sockaddr_in a;
    int err = get_addr(uaddr, &a);
    if (err) return err;
    mutex_lock(&net_lock);
    int64_t n = dgram_send(s, (const void *)ubuf, len, a.addr, ntohs(a.port));
    mutex_unlock(&net_lock);
    return n;
}

int64_t sys_recvfrom(int fd, uint64_t ubuf, uint64_t len, uint64_t uaddr) {
    struct socket *s = fd_socket(fd);
    if (!s) return -ENOTSOCK;
    if (len > IO_MAX) len = IO_MAX;
    if (!user_range_ok(ubuf, len, 1)) return -EFAULT;
    if (uaddr && !user_range_ok(uaddr, sizeof(struct atos_sockaddr_in), 1)) return -EFAULT;
    if (s->type == ATOS_SOCK_STREAM) {
        int64_t n = vfs_read(fd_get(sched_current(), fd), (void *)ubuf, len);
        if (n >= 0 && uaddr) {
            struct atos_sockaddr_in a = {.family = ATOS_AF_INET, .port = htons(s->rport), .addr = s->rip};
            copy_to_user(uaddr, &a, sizeof(a));
        }
        return n;
    }
    uint32_t src = 0;
    uint16_t sport = 0;
    mutex_lock(&net_lock);
    int64_t n = dgram_recv(s, (void *)ubuf, len, &src, &sport);
    mutex_unlock(&net_lock);
    if (n >= 0 && uaddr) {
        struct atos_sockaddr_in a = {.family = ATOS_AF_INET, .port = htons(sport), .addr = src};
        copy_to_user(uaddr, &a, sizeof(a));
    }
    return n;
}

int64_t sys_sockopt(int fd, int opt, uint64_t value) {
    struct socket *s = fd_socket(fd);
    if (!s) return -ENOTSOCK;
    if (opt != ATOS_SO_RCVTIMEO) return -EINVAL;
    s->timeout_ms = value;
    return 0;
}

int64_t sys_netinfo(uint64_t uinfo) {
    struct netif *nif = net_interface();
    if (!nif) return -ENODEV;
    struct atos_netinfo info = {0};
    mutex_lock(&net_lock);
    memcpy(info.mac, nif->mac, ETH_ALEN);
    info.addr = nif->ip;
    info.netmask = nif->netmask;
    info.gateway = nif->gateway;
    info.dns = nif->dns;
    info.flags = dhcp_bound() ? ATOS_NETINFO_DHCP : 0;
    info.rx_packets = nif->rx_packets;
    info.tx_packets = nif->tx_packets;
    info.rx_bytes = nif->rx_bytes;
    info.tx_bytes = nif->tx_bytes;
    info.rx_dropped = nif->rx_dropped;
    mutex_unlock(&net_lock);
    return copy_to_user(uinfo, &info, sizeof(info));
}

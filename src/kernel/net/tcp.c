#include "net.h"
#include "socket.h"
#include <atos/abi.h>
#include "../dev/timer.h"
#include "../lib/string.h"
#include "../mm/heap.h"

/* A deliberately small TCP (RFC 793 + 1122 essentials): active and passive
 * open, in-order receive (out-of-order segments are dropped and re-ACKed,
 * so the peer retransmits), go-back-N retransmission with exponential
 * backoff, zero-window probes, and the full FIN handshake with a short
 * TIME_WAIT. No congestion control, SACK, or window scaling: the window
 * is at most one 16 KiB buffer, and the one peer (QEMU's NAT) is local. */

#define TCP_BUF_SIZE     16384
#define TCP_MSS          (ETH_MTU - IP_HLEN - 20)
#define TCP_DEFAULT_MSS  536
#define TCP_RTO_INITIAL  TIMER_HZ          /* 1 s */
#define TCP_RTO_MAX      (8 * TIMER_HZ)
#define TCP_MAX_RETRIES  6
#define TCP_TIME_WAIT    (2 * TIMER_HZ)    /* 2 MSL shortened: nobody here reuses 4-tuples fast */
#define TCP_ORPHAN_LIFE  (30 * TIMER_HZ)   /* closed by the user but never finished */
#define TCP_BACKLOG_MAX  16

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10

struct tcp_header {
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t off;   /* header length in words, in the top nibble */
    uint8_t flags;
    uint16_t window, checksum, urgent;
} __attribute__((packed));

enum tcp_state {
    CLOSED, LISTEN, SYN_SENT, SYN_RCVD, ESTABLISHED,
    FIN_WAIT_1, FIN_WAIT_2, CLOSE_WAIT, CLOSING, LAST_ACK, TIME_WAIT,
};

struct tcp_conn {
    enum tcp_state state;
    uint32_t lip, rip;       /* network order */
    uint16_t lport, rport;   /* host order */

    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint32_t irs, rcv_nxt;
    uint16_t mss;            /* the peer's */

    /* Send buffer: sndbuf[0] is the byte at snd_una. It holds both
     * unacknowledged and not-yet-sent data. */
    uint8_t *sndbuf;
    uint32_t snd_len;
    int fin_queued, fin_sent, fin_acked;

    /* Receive buffer: in-order data not yet read by the user. */
    uint8_t *rcvbuf;
    uint32_t rcv_len;
    uint32_t last_adv_wnd;
    int peer_fin;

    int error;               /* -ECONNRESET etc., once the connection failed */
    uint64_t rto, rto_deadline;
    int retries;
    uint64_t state_deadline; /* TIME_WAIT's end, or an orphan's */

    /* Passive open: a child connection belongs to its listener until
     * accept() gives it a socket. */
    struct tcp_conn *listener;
    int backlog;
    uint64_t created;

    struct socket *sock;
    struct tcp_conn *next;
};

static struct tcp_conn *conns;

static int seq_lt(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }
static int seq_le(uint32_t a, uint32_t b) { return (int32_t)(a - b) <= 0; }
static uint32_t min32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static uint32_t new_iss(void) {
    static uint32_t salt;
    salt += 0x9E3779B9u;
    return (uint32_t)timer_ticks() * 250000u + salt;
}

static struct tcp_conn *conn_new(int buffers) {
    static uint64_t serial;
    struct tcp_conn *c = kmalloc(sizeof(*c));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    if (buffers) {
        c->sndbuf = kmalloc(TCP_BUF_SIZE);
        c->rcvbuf = kmalloc(TCP_BUF_SIZE);
        if (!c->sndbuf || !c->rcvbuf) {
            kfree(c->sndbuf);
            kfree(c->rcvbuf);
            kfree(c);
            return NULL;
        }
    }
    c->mss = TCP_DEFAULT_MSS;
    c->rto = TCP_RTO_INITIAL;
    c->created = serial++;
    c->next = conns;
    conns = c;
    return c;
}

static void conn_free(struct tcp_conn *c) {
    kfree(c->sndbuf);
    kfree(c->rcvbuf);
    kfree(c);
}

/* Wakes the task that cares: the socket's owner, or for a connection not
 * yet accepted, whoever waits in accept() on its listener. */
static void notify(struct tcp_conn *c) {
    if (c->sock) socket_notify(c->sock);
    else if (c->listener && c->listener->sock) socket_notify(c->listener->sock);
}

static void send_segment(struct tcp_conn *c, uint32_t seq, uint8_t flags,
                         const uint8_t *data, uint32_t len) {
    uint8_t buf[ETH_MTU - IP_HLEN];
    size_t hlen = sizeof(struct tcp_header) + ((flags & F_SYN) ? 4 : 0);
    uint32_t wnd = c->rcvbuf ? TCP_BUF_SIZE - c->rcv_len : 0;
    struct tcp_header h = {
        .sport = htons(c->lport), .dport = htons(c->rport),
        .seq = htonl(seq), .ack = (flags & F_ACK) ? htonl(c->rcv_nxt) : 0,
        .off = (uint8_t)((hlen / 4) << 4), .flags = flags,
        .window = htons((uint16_t)min32(wnd, 0xFFFF)),
    };
    c->last_adv_wnd = wnd;
    memcpy(buf, &h, sizeof(h));
    if (flags & F_SYN) { /* our MSS option */
        buf[20] = 2;
        buf[21] = 4;
        buf[22] = (uint8_t)(TCP_MSS >> 8);
        buf[23] = (uint8_t)TCP_MSS;
    }
    if (len) memcpy(buf + hlen, data, len);
    uint16_t total = (uint16_t)(hlen + len);
    uint32_t sum = checksum_pseudo(c->lip, c->rip, IP_PROTO_TCP, total);
    uint16_t csum = checksum_finish(checksum_add(sum, buf, total));
    memcpy(buf + 16, &csum, 2);
    ipv4_send(c->rip, IP_PROTO_TCP, buf, total);
}

static void send_ack(struct tcp_conn *c) { send_segment(c, c->snd_nxt, F_ACK, NULL, 0); }

/* RFC 793's reply to a segment for no connection. */
static void send_reset(uint32_t src, uint32_t dst, const struct tcp_header *in, uint32_t seg_len) {
    if (in->flags & F_RST) return;
    struct tcp_conn tmp = {
        .lip = dst, .rip = src, .lport = ntohs(in->dport), .rport = ntohs(in->sport),
    };
    if (in->flags & F_ACK) {
        send_segment(&tmp, ntohl(in->ack), F_RST, NULL, 0);
    } else {
        tmp.rcv_nxt = ntohl(in->seq) + seg_len;
        send_segment(&tmp, 0, F_RST | F_ACK, NULL, 0);
    }
}

static void arm_rto(struct tcp_conn *c) {
    if (!c->rto_deadline) c->rto_deadline = timer_ticks() + c->rto;
}

static void conn_fail(struct tcp_conn *c, int err) {
    c->error = err;
    c->state = CLOSED;
    c->rto_deadline = 0;
    notify(c);
}

static int can_send_data(const struct tcp_conn *c) {
    return c->state == ESTABLISHED || c->state == CLOSE_WAIT ||
           c->state == FIN_WAIT_1 || c->state == CLOSING || c->state == LAST_ACK;
}

/* Sends whatever buffered data the peer's window allows, then our FIN
 * once the user has closed and everything before it is out. */
static void tcp_output(struct tcp_conn *c) {
    if (!can_send_data(c)) return;
    while (!c->fin_sent) {
        uint32_t inflight = c->snd_nxt - c->snd_una;
        uint32_t unsent = c->snd_len - inflight;
        uint32_t room = c->snd_wnd > inflight ? c->snd_wnd - inflight : 0;
        uint32_t n = min32(min32(unsent, c->mss), room);
        if (n == 0) {
            if (unsent == 0 && c->fin_queued) {
                send_segment(c, c->snd_nxt, F_FIN | F_ACK, NULL, 0);
                c->snd_nxt++;
                c->fin_sent = 1;
                arm_rto(c);
            }
            return;
        }
        send_segment(c, c->snd_nxt, F_ACK | F_PSH, c->sndbuf + inflight, n);
        c->snd_nxt += n;
        arm_rto(c);
    }
}

static void retransmit(struct tcp_conn *c) {
    c->rto_deadline = 0;
    if (++c->retries > TCP_MAX_RETRIES) {
        if (c->state != SYN_SENT) send_segment(c, c->snd_nxt, F_RST | F_ACK, NULL, 0);
        conn_fail(c, -ETIMEDOUT);
        return;
    }
    c->rto = c->rto * 2 < TCP_RTO_MAX ? c->rto * 2 : TCP_RTO_MAX;
    if (c->state == SYN_SENT) {
        send_segment(c, c->iss, F_SYN, NULL, 0);
        arm_rto(c);
        return;
    }
    if (c->state == SYN_RCVD) {
        send_segment(c, c->iss, F_SYN | F_ACK, NULL, 0);
        arm_rto(c);
        return;
    }
    /* Go back N: resend everything from the oldest unacknowledged byte. */
    c->snd_nxt = c->snd_una;
    if (!c->fin_acked) c->fin_sent = 0;
    if (c->snd_wnd == 0 && c->snd_len) {
        /* Zero-window probe: one byte, so the peer's ACK reports when
         * its window reopens. */
        send_segment(c, c->snd_nxt, F_ACK, c->sndbuf, 1);
        c->snd_nxt++;
        arm_rto(c);
        return;
    }
    tcp_output(c);
}

static uint16_t parse_mss(const uint8_t *opt, size_t len) {
    size_t i = 0;
    while (i < len && opt[i] != 0) {
        if (opt[i] == 1) { i++; continue; } /* NOP */
        if (i + 1 >= len || opt[i + 1] < 2 || i + opt[i + 1] > len) break;
        if (opt[i] == 2 && opt[i + 1] == 4) {
            uint16_t mss = (uint16_t)(opt[i + 2] << 8 | opt[i + 3]);
            if (mss > TCP_MSS) mss = TCP_MSS;
            return mss < 64 ? 64 : mss;
        }
        i += opt[i + 1];
    }
    return TCP_DEFAULT_MSS;
}

static unsigned children_pending(const struct tcp_conn *listener) {
    unsigned n = 0;
    for (struct tcp_conn *c = conns; c; c = c->next) {
        if (c->listener == listener && c->state != CLOSED) n++;
    }
    return n;
}

static void listen_input(struct tcp_conn *l, uint32_t src, uint32_t dst,
                         const struct tcp_header *h, const uint8_t *opts, size_t optlen,
                         uint32_t seg_len) {
    if (h->flags & F_RST) return;
    if (h->flags & F_ACK) {
        send_reset(src, dst, h, seg_len);
        return;
    }
    if (!(h->flags & F_SYN)) return;
    if (children_pending(l) >= (unsigned)l->backlog) return; /* the peer will retry */

    struct tcp_conn *c = conn_new(1);
    if (!c) return;
    c->state = SYN_RCVD;
    c->lip = dst;
    c->rip = src;
    c->lport = l->lport;
    c->rport = ntohs(h->sport);
    c->irs = ntohl(h->seq);
    c->rcv_nxt = c->irs + 1;
    c->iss = new_iss();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->snd_wnd = ntohs(h->window);
    c->mss = parse_mss(opts, optlen);
    c->listener = l;
    send_segment(c, c->iss, F_SYN | F_ACK, NULL, 0);
    arm_rto(c);
}

static void syn_sent_input(struct tcp_conn *c, uint32_t src, uint32_t dst,
                           const struct tcp_header *h, const uint8_t *opts, size_t optlen,
                           uint32_t seg_len) {
    uint32_t ack = ntohl(h->ack);
    int ack_ok = (h->flags & F_ACK) && ack == c->iss + 1;
    if ((h->flags & F_ACK) && !ack_ok) {
        send_reset(src, dst, h, seg_len);
        return;
    }
    if (h->flags & F_RST) {
        if (ack_ok) conn_fail(c, -ECONNREFUSED);
        return;
    }
    if (!(h->flags & F_SYN)) return;

    c->irs = ntohl(h->seq);
    c->rcv_nxt = c->irs + 1;
    c->mss = parse_mss(opts, optlen);
    c->snd_wnd = ntohs(h->window);
    if (ack_ok) {
        c->snd_una = ack;
        c->state = ESTABLISHED;
        c->rto_deadline = 0;
        c->retries = 0;
        c->rto = TCP_RTO_INITIAL;
        send_ack(c);
        notify(c);
    } else { /* simultaneous open */
        c->state = SYN_RCVD;
        send_segment(c, c->iss, F_SYN | F_ACK, NULL, 0);
    }
}

/* Processes an acceptable ACK: frees acknowledged send-buffer space and
 * advances the FIN handshake. */
static void process_ack(struct tcp_conn *c, uint32_t ack, uint16_t window) {
    if (seq_lt(c->snd_nxt, ack)) { /* acks something never sent */
        send_ack(c);
        return;
    }
    if (seq_lt(ack, c->snd_una)) return; /* old duplicate */
    c->snd_wnd = window;
    if (ack == c->snd_una) return;

    uint32_t acked = ack - c->snd_una;
    uint32_t data = min32(acked, c->snd_len);
    memmove(c->sndbuf, c->sndbuf + data, c->snd_len - data);
    c->snd_len -= data;
    if (acked > data) c->fin_acked = 1;
    c->snd_una = ack;
    c->retries = 0;
    c->rto = TCP_RTO_INITIAL;
    c->rto_deadline = c->snd_una == c->snd_nxt ? 0 : timer_ticks() + c->rto;
    notify(c); /* send-buffer space, or a finished close */

    if (c->fin_acked) {
        if (c->state == FIN_WAIT_1) {
            c->state = FIN_WAIT_2;
        } else if (c->state == CLOSING) {
            c->state = TIME_WAIT;
            c->state_deadline = timer_ticks() + TCP_TIME_WAIT;
        } else if (c->state == LAST_ACK) {
            c->state = CLOSED;
        }
    }
}

static struct tcp_conn *find_conn(uint32_t src, uint16_t sport, uint16_t dport) {
    struct tcp_conn *listener = NULL;
    for (struct tcp_conn *c = conns; c; c = c->next) {
        if (c->state == CLOSED || c->lport != dport) continue;
        if (c->state == LISTEN) listener = c;
        else if (c->rip == src && c->rport == sport) return c;
    }
    return listener;
}

void tcp_input(uint32_t src, uint32_t dst, const uint8_t *seg, size_t len) {
    struct tcp_header h;
    if (len < sizeof(h)) return;
    memcpy(&h, seg, sizeof(h));
    size_t hlen = (size_t)(h.off >> 4) * 4;
    if (hlen < sizeof(h) || hlen > len) return;
    uint32_t sum = checksum_pseudo(src, dst, IP_PROTO_TCP, (uint16_t)len);
    if (checksum_finish(checksum_add(sum, seg, len)) != 0) return;

    const uint8_t *opts = seg + sizeof(h);
    size_t optlen = hlen - sizeof(h);
    const uint8_t *data = seg + hlen;
    uint32_t dlen = (uint32_t)(len - hlen);
    uint32_t seg_len = dlen + ((h.flags & F_SYN) ? 1 : 0) + ((h.flags & F_FIN) ? 1 : 0);
    uint32_t seq = ntohl(h.seq);

    struct tcp_conn *c = find_conn(src, ntohs(h.sport), ntohs(h.dport));
    if (!c) {
        send_reset(src, dst, &h, seg_len);
        return;
    }
    if (c->state == LISTEN) {
        listen_input(c, src, dst, &h, opts, optlen, seg_len);
        return;
    }
    if (c->state == SYN_SENT) {
        syn_sent_input(c, src, dst, &h, opts, optlen, seg_len);
        return;
    }

    /* A synchronized state (SYN_RCVD onward). */
    if (h.flags & F_RST) {
        /* Only a reset at the expected sequence number counts (RFC 5961's
         * simplest form), so a stray one can't kill the connection. */
        if (seq == c->rcv_nxt) conn_fail(c, -ECONNRESET);
        return;
    }
    if (h.flags & F_SYN) {
        if (c->state == SYN_RCVD && seq == c->irs) {
            send_segment(c, c->iss, F_SYN | F_ACK, NULL, 0); /* our SYN-ACK was lost */
        } else {
            send_ack(c);
        }
        return;
    }
    if (!(h.flags & F_ACK)) return;

    uint32_t ack = ntohl(h.ack);
    if (c->state == SYN_RCVD) {
        if (ack != c->iss + 1) {
            send_reset(src, dst, &h, seg_len);
            return;
        }
        c->state = ESTABLISHED;
        c->snd_una = ack;
        c->snd_wnd = ntohs(h.window);
        c->rto_deadline = 0;
        c->retries = 0;
        c->rto = TCP_RTO_INITIAL;
        notify(c); /* ready for accept() */
    } else {
        process_ack(c, ack, ntohs(h.window));
        if (c->state == CLOSED) return;
    }

    int need_ack = 0;
    int in_order = seq_le(seq, c->rcv_nxt);
    if (dlen) {
        need_ack = 1;
        int receiving = c->state == ESTABLISHED || c->state == FIN_WAIT_1 || c->state == FIN_WAIT_2;
        if (receiving && in_order) {
            uint32_t skip = c->rcv_nxt - seq; /* already-received overlap */
            if (skip < dlen) {
                uint32_t n = min32(dlen - skip, TCP_BUF_SIZE - c->rcv_len);
                memcpy(c->rcvbuf + c->rcv_len, data + skip, n);
                c->rcv_len += n;
                c->rcv_nxt += n;
                if (n) notify(c);
            }
        }
    }

    if (h.flags & F_FIN) {
        need_ack = 1;
        if (seq + dlen == c->rcv_nxt && !c->peer_fin) {
            c->rcv_nxt++;
            c->peer_fin = 1;
            notify(c);
            switch (c->state) {
            case ESTABLISHED: c->state = CLOSE_WAIT; break;
            case FIN_WAIT_1:
                c->state = c->fin_acked ? TIME_WAIT : CLOSING;
                break;
            case FIN_WAIT_2: c->state = TIME_WAIT; break;
            default: break;
            }
            if (c->state == TIME_WAIT) c->state_deadline = timer_ticks() + TCP_TIME_WAIT;
        } else if (c->state == TIME_WAIT) {
            c->state_deadline = timer_ticks() + TCP_TIME_WAIT; /* our last ACK was lost */
        }
    }

    if (need_ack) send_ack(c);
    tcp_output(c);
}

void tcp_tick(uint64_t now) {
    for (struct tcp_conn **pp = &conns; *pp;) {
        struct tcp_conn *c = *pp;
        if (c->rto_deadline && now >= c->rto_deadline) retransmit(c);
        if (c->state == TIME_WAIT && now >= c->state_deadline) {
            c->state = CLOSED;
        } else if (!c->sock && !c->listener && c->state != CLOSED &&
                   c->state_deadline && now >= c->state_deadline) {
            send_segment(c, c->snd_nxt, F_RST | F_ACK, NULL, 0); /* orphan gave up */
            c->state = CLOSED;
        }
        if (c->state == CLOSED && !c->sock) {
            *pp = c->next;
            conn_free(c);
        } else {
            pp = &c->next;
        }
    }
}

/* --- the socket interface; net_lock held --- */

int tcp_sock_connect(struct socket *s, uint32_t dst, uint16_t dport) {
    if (s->conn) return s->conn->state == LISTEN ? -EINVAL : -EISCONN;
    struct netif *nif = net_interface();
    struct tcp_conn *c = conn_new(1);
    if (!c) return -ENOMEM;
    if (!s->lport) s->lport = socket_ephemeral_port(ATOS_SOCK_STREAM);
    c->lip = nif->ip;
    c->rip = dst;
    c->lport = s->lport;
    c->rport = dport;
    c->state = SYN_SENT;
    c->iss = new_iss();
    c->snd_una = c->iss;
    c->snd_nxt = c->iss + 1;
    c->sock = s;
    s->conn = c;
    s->rip = dst;
    s->rport = dport;
    send_segment(c, c->iss, F_SYN, NULL, 0);
    arm_rto(c);

    int err = 0;
    socket_deadline_start(s);
    while (c->state == SYN_SENT && !err) err = socket_wait(s);
    socket_deadline_end(s);
    if (!err && c->state != CLOSED) {
        s->connected = 1;
        return 0;
    }
    if (!err) err = c->error ? c->error : -ECONNREFUSED;
    /* Give the connection up; tcp_tick frees it. */
    c->state = CLOSED;
    c->rto_deadline = 0;
    c->sock = NULL;
    s->conn = NULL;
    return err;
}

int tcp_sock_listen(struct socket *s, int backlog) {
    if (s->conn) return s->conn->state == LISTEN ? 0 : -EISCONN;
    if (!s->lport) return -EINVAL; /* bind() first */
    struct tcp_conn *c = conn_new(0);
    if (!c) return -ENOMEM;
    c->state = LISTEN;
    c->lport = s->lport;
    c->backlog = backlog < 1 ? 1 : backlog > TCP_BACKLOG_MAX ? TCP_BACKLOG_MAX : backlog;
    c->sock = s;
    s->conn = c;
    return 0;
}

struct socket *tcp_sock_accept(struct socket *s, int *err) {
    struct tcp_conn *l = s->conn;
    if (!l || l->state != LISTEN) {
        *err = -EINVAL;
        return NULL;
    }
    struct tcp_conn *child;
    socket_deadline_start(s);
    for (;;) {
        child = NULL;
        for (struct tcp_conn *c = conns; c; c = c->next) {
            if (c->listener == l && c->state != SYN_RCVD && c->state != CLOSED &&
                (!child || c->created < child->created)) {
                child = c;
            }
        }
        if (child) break;
        int e = socket_wait(s);
        if (e) {
            socket_deadline_end(s);
            *err = e;
            return NULL;
        }
    }
    socket_deadline_end(s);

    struct socket *ns = socket_new(ATOS_SOCK_STREAM);
    if (!ns) {
        *err = -ENOMEM;
        return NULL;
    }
    ns->lport = child->lport;
    ns->rip = child->rip;
    ns->rport = child->rport;
    ns->connected = 1;
    ns->conn = child;
    child->sock = ns;
    child->listener = NULL;
    return ns;
}

int64_t tcp_sock_send(struct socket *s, const void *buf, uint64_t len) {
    struct tcp_conn *c = s->conn;
    if (!c || c->state == LISTEN) return -ENOTCONN;
    const uint8_t *p = buf;
    uint64_t done = 0;
    while (done < len) {
        if (c->error) return done ? (int64_t)done : c->error;
        if (c->state != ESTABLISHED && c->state != CLOSE_WAIT) return done ? (int64_t)done : -EPIPE;
        uint32_t room = TCP_BUF_SIZE - c->snd_len;
        if (room == 0) {
            socket_wait(s); /* no deadline: sends wait for the peer's ACKs */
            continue;
        }
        uint32_t n = (uint32_t)(len - done < room ? len - done : room);
        memcpy(c->sndbuf + c->snd_len, p + done, n);
        c->snd_len += n;
        done += n;
        tcp_output(c);
    }
    return (int64_t)done;
}

int64_t tcp_sock_recv(struct socket *s, void *buf, uint64_t len) {
    struct tcp_conn *c = s->conn;
    if (!c || c->state == LISTEN) return -ENOTCONN;
    int64_t ret;
    socket_deadline_start(s);
    for (;;) {
        if (c->rcv_len) {
            uint32_t n = (uint32_t)(len < c->rcv_len ? len : c->rcv_len);
            memcpy(buf, c->rcvbuf, n);
            memmove(c->rcvbuf, c->rcvbuf + n, c->rcv_len - n);
            c->rcv_len -= n;
            /* Tell a peer throttled by our window that it reopened. */
            if (c->last_adv_wnd <= TCP_BUF_SIZE / 2 && c->state != CLOSED) send_ack(c);
            ret = n;
            break;
        }
        if (c->peer_fin || (c->state == CLOSED && !c->error)) { ret = 0; break; }
        if (c->error) { ret = c->error; break; }
        int err = socket_wait(s);
        if (err) { ret = err; break; }
    }
    socket_deadline_end(s);
    return ret;
}

void tcp_sock_close(struct socket *s) {
    struct tcp_conn *c = s->conn;
    if (!c) return;
    s->conn = NULL;
    c->sock = NULL;
    switch (c->state) {
    case LISTEN:
        for (struct tcp_conn *child = conns; child; child = child->next) {
            if (child->listener != c) continue;
            if (child->state != CLOSED) send_segment(child, child->snd_nxt, F_RST | F_ACK, NULL, 0);
            child->state = CLOSED;
            child->rto_deadline = 0;
            child->listener = NULL;
        }
        c->state = CLOSED;
        return;
    case SYN_SENT:
        c->state = CLOSED;
        return;
    case SYN_RCVD:
    case ESTABLISHED:
        c->state = FIN_WAIT_1;
        break;
    case CLOSE_WAIT:
        c->state = LAST_ACK;
        break;
    default:
        return;
    }
    c->fin_queued = 1;
    c->state_deadline = timer_ticks() + TCP_ORPHAN_LIFE;
    tcp_output(c);
}

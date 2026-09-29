#include <arpa/inet.h>
#include <atos.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int h_errno;

int inet_aton(const char *s, struct in_addr *out) {
    uint32_t addr = 0;
    for (int part = 0; part < 4; part++) {
        if (*s < '0' || *s > '9') return 0;
        uint32_t v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (uint32_t)(*s++ - '0');
            if (v > 255) return 0;
        }
        addr |= v << (8 * part); /* network order: first octet lowest in memory */
        if (part < 3 && *s++ != '.') return 0;
    }
    if (*s) return 0;
    out->s_addr = addr;
    return 1;
}

in_addr_t inet_addr(const char *text) {
    struct in_addr a;
    return inet_aton(text, &a) ? a.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr addr) {
    static char buf[16];
    char *p = buf;
    for (int i = 0; i < 4; i++) {
        unsigned v = (addr.s_addr >> (8 * i)) & 0xFF;
        if (v >= 100) *p++ = (char)('0' + v / 100);
        if (v >= 10) *p++ = (char)('0' + v / 10 % 10);
        *p++ = (char)('0' + v % 10);
        *p++ = i < 3 ? '.' : '\0';
    }
    return buf;
}

/* --- DNS (RFC 1035): one A query over UDP --- */

#define DNS_TYPE_A  1
#define DNS_CLASS_IN 1
#define DNS_MAX 512

/* Appends `name` as length-prefixed labels; -1 if a label is empty or
 * too long. */
static int put_name(uint8_t *out, size_t cap, const char *name) {
    size_t pos = 0;
    while (*name) {
        const char *dot = strchr(name, '.');
        size_t len = dot ? (size_t)(dot - name) : strlen(name);
        if (len == 0 || len > 63 || pos + len + 2 > cap) return -1;
        out[pos++] = (uint8_t)len;
        memcpy(out + pos, name, len);
        pos += len;
        name += len;
        if (*name == '.') name++;
    }
    out[pos++] = 0;
    return (int)pos;
}

/* Skips a possibly compressed name at msg[pos]; the offset after it, or -1. */
static int skip_name(const uint8_t *msg, int len, int pos) {
    while (pos < len) {
        uint8_t l = msg[pos];
        if (l == 0) return pos + 1;
        if ((l & 0xC0) == 0xC0) return pos + 2 <= len ? pos + 2 : -1; /* pointer ends it */
        pos += l + 1;
    }
    return -1;
}

static int parse_reply(const uint8_t *msg, int len, uint16_t id, uint32_t *out) {
    if (len < 12 || (msg[0] << 8 | msg[1]) != id || !(msg[2] & 0x80)) return -EAGAIN;
    int rcode = msg[3] & 0x0F;
    if (rcode == 3) return -ENOENT; /* NXDOMAIN */
    if (rcode != 0) return -EIO;
    int qd = msg[4] << 8 | msg[5], an = msg[6] << 8 | msg[7];
    int pos = 12;
    for (int i = 0; i < qd; i++) {
        pos = skip_name(msg, len, pos);
        if (pos < 0 || pos + 4 > len) return -EIO;
        pos += 4;
    }
    for (int i = 0; i < an; i++) {
        pos = skip_name(msg, len, pos);
        if (pos < 0 || pos + 10 > len) return -EIO;
        int type = msg[pos] << 8 | msg[pos + 1];
        int cls = msg[pos + 2] << 8 | msg[pos + 3];
        int rdlen = msg[pos + 8] << 8 | msg[pos + 9];
        pos += 10;
        if (pos + rdlen > len) return -EIO;
        if (type == DNS_TYPE_A && cls == DNS_CLASS_IN && rdlen == 4) {
            memcpy(out, msg + pos, 4);
            return 0;
        }
        pos += rdlen; /* a CNAME or similar: the A record follows */
    }
    return -ENOENT;
}

int dns_resolve(const char *name, uint32_t server, uint16_t port, int timeout_ms, uint32_t *out) {
    static uint16_t next_id = 0x4154; /* "AT" */
    uint8_t q[DNS_MAX];
    uint16_t id = next_id++;
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01; /* recursion desired */
    q[5] = 1;    /* one question */
    int n = put_name(q + 12, sizeof(q) - 16, name);
    if (n < 0) {
        errno = EINVAL;
        return -1;
    }
    int qlen = 12 + n;
    q[qlen++] = 0;
    q[qlen++] = DNS_TYPE_A;
    q[qlen++] = 0;
    q[qlen++] = DNS_CLASS_IN;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr = {server}};

    int err = -ETIMEDOUT;
    for (int attempt = 0; attempt < 3 && err == -ETIMEDOUT; attempt++) {
        if (sendto(fd, q, (size_t)qlen, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
            err = -errno;
            break;
        }
        for (;;) { /* skip stray datagrams until ours or the timeout */
            uint8_t r[DNS_MAX];
            long got = recv(fd, r, sizeof(r), 0);
            if (got < 0) {
                err = -errno;
                break;
            }
            err = parse_reply(r, (int)got, id, out);
            if (err != -EAGAIN) break;
        }
    }
    close(fd);
    if (err) {
        errno = -err;
        return -1;
    }
    return 0;
}

struct hostent *gethostbyname(const char *name) {
    static struct hostent ent;
    static uint32_t addr;
    static char *addrs[2];
    static char *aliases[1];
    static char namebuf[256];

    struct in_addr a;
    if (inet_aton(name, &a)) {
        addr = a.s_addr;
    } else {
        struct atos_netinfo info;
        if (netinfo(&info) < 0 || dns_resolve(name, info.dns, 53, 2000, &addr) < 0) {
            h_errno = errno == ENOENT ? HOST_NOT_FOUND : TRY_AGAIN;
            return NULL;
        }
    }
    size_t len = strlen(name);
    if (len >= sizeof(namebuf)) len = sizeof(namebuf) - 1;
    memcpy(namebuf, name, len);
    namebuf[len] = '\0';
    addrs[0] = (char *)&addr;
    addrs[1] = NULL;
    aliases[0] = NULL;
    ent.h_name = namebuf;
    ent.h_aliases = aliases;
    ent.h_addrtype = AF_INET;
    ent.h_length = 4;
    ent.h_addr_list = addrs;
    return &ent;
}

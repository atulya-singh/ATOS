#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* wget [-O file] http://host[:port]/path -- an HTTP/1.0 GET. The body goes
 * to `file` (default: the URL's last path component, or index.html; "-"
 * is standard output). */

static int write_all(int fd, const char *p, long n) {
    while (n > 0) {
        long w = write(fd, p, (size_t)n);
        if (w <= 0) return -1;
        p += w;
        n -= w;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *out = NULL, *url;
    if (argc == 4 && strcmp(argv[1], "-O") == 0) {
        out = argv[2];
        url = argv[3];
    } else if (argc == 2) {
        url = argv[1];
    } else {
        dprintf(STDERR_FILENO, "usage: wget [-O file] http://host[:port]/path\n");
        return 2;
    }
    if (strncmp(url, "http://", 7) != 0) {
        dprintf(STDERR_FILENO, "wget: only http:// URLs are supported\n");
        return 2;
    }

    char host[128];
    const char *p = url + 7;
    size_t hl = 0;
    while (*p && *p != '/' && *p != ':' && hl < sizeof(host) - 1) host[hl++] = *p++;
    host[hl] = '\0';
    int port = 80;
    if (*p == ':') {
        port = atoi(++p);
        while (*p >= '0' && *p <= '9') p++;
    }
    const char *path = *p ? p : "/";
    if (!hl || port <= 0 || port > 65535 || *path != '/') {
        dprintf(STDERR_FILENO, "wget: bad URL '%s'\n", url);
        return 2;
    }
    if (!out) {
        const char *base = strrchr(path, '/') + 1;
        out = *base ? base : "index.html";
    }

    struct hostent *h = gethostbyname(host);
    if (!h) {
        dprintf(STDERR_FILENO, "wget: %s: unknown host\n", host);
        return 1;
    }
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    memcpy(&to.sin_addr, h->h_addr, 4);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&to, sizeof(to)) < 0) {
        dprintf(STDERR_FILENO, "wget: connect to %s:%d: %s\n", host, port, strerror(errno));
        return 1;
    }
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: ATOS-wget\r\nConnection: close\r\n\r\n",
                      path, host);
    if (rl >= (int)sizeof(req) || write_all(fd, req, rl) < 0) {
        dprintf(STDERR_FILENO, "wget: sending the request failed\n");
        return 1;
    }

    /* Collect the header (up to the blank line), then stream the body. */
    char head[2048];
    size_t hlen = 0;
    char *body = NULL;
    while (!body) {
        if (hlen == sizeof(head) - 1) {
            dprintf(STDERR_FILENO, "wget: response header too long\n");
            return 1;
        }
        long n = read(fd, head + hlen, sizeof(head) - 1 - hlen);
        if (n <= 0) {
            dprintf(STDERR_FILENO, "wget: %s\n", n < 0 ? strerror(errno) : "connection closed early");
            return 1;
        }
        hlen += (size_t)n;
        head[hlen] = '\0';
        char *end = NULL;
        for (size_t i = 0; i + 3 < hlen; i++) {
            if (memcmp(head + i, "\r\n\r\n", 4) == 0) { end = head + i; break; }
        }
        if (end) body = end + 4;
    }
    int status = 0;
    if (strncmp(head, "HTTP/1.", 7) == 0 && head[8] == ' ') status = atoi(head + 9);
    if (status != 200) {
        char *eol = strchr(head, '\r');
        if (eol) *eol = '\0';
        dprintf(STDERR_FILENO, "wget: server said: %s\n", head);
        return 1;
    }

    int ofd = strcmp(out, "-") == 0 ? STDOUT_FILENO : open(out, O_WRONLY | O_CREAT | O_TRUNC);
    if (ofd < 0) {
        dprintf(STDERR_FILENO, "wget: %s: %s\n", out, strerror(errno));
        return 1;
    }
    unsigned long total = (unsigned long)(head + hlen - body);
    if (write_all(ofd, body, (long)total) < 0) goto write_failed;
    static char buf[8192];
    long n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (write_all(ofd, buf, n) < 0) goto write_failed;
        total += (unsigned long)n;
    }
    if (n < 0) {
        dprintf(STDERR_FILENO, "wget: %s\n", strerror(errno));
        return 1;
    }
    close(fd);
    if (ofd != STDOUT_FILENO) {
        close(ofd);
        printf("wget: saved %lu bytes to %s\n", total, out);
    }
    return 0;

write_failed:
    dprintf(STDERR_FILENO, "wget: writing %s: %s\n", out, strerror(errno));
    return 1;
}

#include <arpa/inet.h>
#include <atos.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* httpd [-n count] [port]: a tiny HTTP/1.0 server for the filesystem.
 * Files are sent as-is, directories as a plain-text listing. Serves
 * `count` requests and exits (default: forever), one at a time. */

static int write_all(int fd, const char *p, long n) {
    while (n > 0) {
        long w = write(fd, p, (size_t)n);
        if (w <= 0) return -1;
        p += w;
        n -= w;
    }
    return 0;
}

static void reply(int c, int status, const char *reason, const char *body) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.0 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %lu\r\n\r\n",
                     status, reason, (unsigned long)strlen(body));
    write_all(c, head, n);
    write_all(c, body, (long)strlen(body));
}

static int serve(int c, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        reply(c, 404, "Not Found", "not found\n");
        return 404;
    }
    struct atos_stat st;
    fstat(fd, &st);
    char head[256];
    if (st.type == ATOS_TYPE_DIR) {
        int n = snprintf(head, sizeof(head),
                         "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n\r\n");
        write_all(c, head, n);
        struct atos_dirent e;
        for (unsigned long i = 0; readdir(fd, i, &e) == 0; i++) {
            n = snprintf(head, sizeof(head), "%s%s\n", e.name, e.type == ATOS_TYPE_DIR ? "/" : "");
            write_all(c, head, n);
        }
    } else {
        int n = snprintf(head, sizeof(head),
                         "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
                         "Content-Length: %lu\r\n\r\n", (unsigned long)st.size);
        write_all(c, head, n);
        static char buf[8192];
        long r;
        while ((r = read(fd, buf, sizeof(buf))) > 0) {
            if (write_all(c, buf, r) < 0) break;
        }
    }
    close(fd);
    return 200;
}

static void handle(int c, struct sockaddr_in *peer) {
    struct timeval tv = {5, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[1024];
    size_t len = 0;
    /* Only the request line matters; read until the header ends. */
    while (len < sizeof(req) - 1) {
        long n = read(c, req + len, sizeof(req) - 1 - len);
        if (n <= 0) break;
        len += (size_t)n;
        req[len] = '\0';
        if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break;
    }
    req[len] = '\0';
    char *sp1 = strchr(req, ' ');
    char *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    int status;
    char path[256] = "?";
    if (!sp1 || !sp2 || (size_t)(sp2 - sp1 - 1) >= sizeof(path)) {
        reply(c, 400, "Bad Request", "bad request\n");
        status = 400;
    } else {
        memcpy(path, sp1 + 1, (size_t)(sp2 - sp1 - 1));
        path[sp2 - sp1 - 1] = '\0';
        if (strncmp(req, "GET ", 4) != 0) {
            reply(c, 501, "Not Implemented", "only GET is supported\n");
            status = 501;
        } else if (path[0] != '/') {
            reply(c, 400, "Bad Request", "bad path\n");
            status = 400;
        } else {
            status = serve(c, path);
        }
    }
    printf("httpd: %s GET %s -> %d\n", inet_ntoa(peer->sin_addr), path, status);
    close(c);
}

int main(int argc, char **argv) {
    int count = -1, port = 80, i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        count = atoi(argv[i + 1]);
        i += 2;
    }
    if (i < argc) port = atoi(argv[i++]);
    if (i != argc || port <= 0 || port > 65535 || count == 0) {
        dprintf(STDERR_FILENO, "usage: httpd [-n count] [port]\n");
        return 2;
    }
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(s, 4) < 0) {
        dprintf(STDERR_FILENO, "httpd: port %d: %s\n", port, strerror(errno));
        return 1;
    }
    printf("httpd: listening on port %d\n", port);
    for (int served = 0; count < 0 || served < count; served++) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int c = accept(s, (struct sockaddr *)&peer, &plen);
        if (c < 0) {
            dprintf(STDERR_FILENO, "httpd: accept: %s\n", strerror(errno));
            return 1;
        }
        handle(c, &peer);
    }
    close(s);
    return 0;
}

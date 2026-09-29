#include "path.h"
#include "string.h"
#include <atos/abi.h>

/* Rewrites an absolute path into canonical form: no empty or "."
 * components, ".." applied lexically (and clamped at the root), no
 * trailing slash except for "/" itself. There is no working directory
 * yet, so relative paths are rejected. */
int path_normalize(const char *path, char *out) {
    if (path[0] != '/') return -ENOENT;
    size_t len = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        size_t clen = (size_t)(p - start);

        if (clen == 0 || (clen == 1 && start[0] == '.')) continue;
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            while (len > 0 && out[len - 1] != '/') len--;
            if (len > 0) len--; /* drop the slash too */
            continue;
        }
        if (clen >= ATOS_NAME_MAX) return -ENAMETOOLONG;
        if (len + 1 + clen >= PATH_MAX_LEN) return -ENAMETOOLONG;
        out[len++] = '/';
        memcpy(out + len, start, clen);
        len += clen;
    }
    if (len == 0) out[len++] = '/';
    out[len] = '\0';
    return 0;
}

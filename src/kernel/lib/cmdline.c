#include "cmdline.h"
#include "string.h"
#include "../mm/boot_info.h"

/* Start of the word beginning with `prefix` of length n, or NULL. */
static const char *find_word(const char *prefix, size_t n, int exact) {
    const char *line = boot_info_cmdline();
    for (const char *p = line; *p; p++) {
        if (p != line && p[-1] != ' ') continue;
        if (memcmp(p, prefix, n) != 0) continue;
        if (!exact || p[n] == ' ' || p[n] == '\0') return p;
    }
    return NULL;
}

int cmdline_has(const char *word) {
    return find_word(word, strlen(word), 1) != NULL;
}

int cmdline_get(const char *key, char *out, size_t size) {
    char prefix[32];
    size_t n = strlen(key);
    if (n + 2 > sizeof(prefix) || size == 0) return 0;
    memcpy(prefix, key, n);
    prefix[n++] = '=';
    const char *p = find_word(prefix, n, 0);
    if (!p) return 0;
    p += n;
    size_t len = 0;
    while (p[len] && p[len] != ' ' && len < size - 1) len++;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

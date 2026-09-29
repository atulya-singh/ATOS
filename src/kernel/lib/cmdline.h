#pragma once
#include <stddef.h>

/* The kernel command line (limine.conf's `cmdline:`), as space-separated
 * words that are either flags ("selftest-exit") or key=value pairs. */

/* Whether the flag `word` is present. */
int cmdline_has(const char *word);

/* Copies the value of `key=...` into `out` (truncated to size-1 chars);
 * 1 if the key was present, else 0 and `out` is left untouched. */
int cmdline_get(const char *key, char *out, size_t size);

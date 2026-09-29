#pragma once

/* Longest normalized path the VFS handles, including the terminator. */
#define PATH_MAX_LEN 256
#define VFS_PATH_MAX PATH_MAX_LEN

/* Rewrites absolute `path` into canonical form in `out` (PATH_MAX_LEN
 * bytes): no empty or "." components, ".." applied lexically and clamped
 * at the root, no trailing slash except for "/" itself. Returns 0,
 * -ENOENT for a relative path, or -ENAMETOOLONG. */
int path_normalize(const char *path, char *out);

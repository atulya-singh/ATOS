#pragma once
#include <stddef.h>
#include <stdint.h>

/* ustar archive format, as parsed by the initrd. Pure data handling, so
 * tests/host can check it against archives made by the host's tar. */

struct tar_header {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6]; /* "ustar\0" (POSIX) or "ustar " (old GNU) */
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} __attribute__((packed));

#define TAR_BLOCK 512
#define TAR_TYPE_FILE  '0'
#define TAR_TYPE_AFILE '\0' /* pre-POSIX regular file */
#define TAR_TYPE_DIR   '5'

/* Numeric header fields are NUL- or space-terminated octal, optionally
 * space-padded in front. */
uint64_t tar_parse_octal(const char *s, size_t n);

/* Verifies a header block's checksum field. */
int tar_checksum_ok(const struct tar_header *h);

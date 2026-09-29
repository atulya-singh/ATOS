#pragma once
#include <stdint.h>

/* NT case flags (the dirent's "reserved" byte): the 8.3 base and/or
 * extension are displayed lowercase. */
#define NTRES_LOWER_BASE 0x08
#define NTRES_LOWER_EXT  0x10

/* Case-insensitive (ASCII) equality, as FAT lookups require. */
int fat_name_eq(const char *a, const char *b);

/* 11-byte 8.3 name -> display form: "FOO     TXT" -> "FOO.TXT", or
 * "foo.txt" per the NT case flags. `out` needs 13 bytes. */
void fat_name_decode(const char raw[11], uint8_t ntres, char *out);

/* The checksum long-name entries carry of their 8.3 entry's name. */
uint8_t fat_name_checksum(const char raw[11]);

/* Fills `out` if `name` is exactly representable as an 8.3 name (with NT
 * flags for an all-lowercase part); 0 if it needs a long-name entry. */
int fat_name_exact(const char *name, char out[11], uint8_t *ntres);

/* The "BASIS~N.EXT" 8.3 alias for a long name. */
void fat_name_alias(const char *name, unsigned n, char out[11]);

/* Whether `name` may be created: printable ASCII, none of \/:*?"<>|,
 * not "." or "..", no trailing space or dot, shorter than ATOS_NAME_MAX. */
int fat_name_valid(const char *name);

/* FAT's packed date and time fields (2-second resolution). */
uint16_t fat_encode_date(unsigned year, unsigned month, unsigned day);
uint16_t fat_encode_time(unsigned hour, unsigned minute, unsigned second);

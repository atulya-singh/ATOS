#pragma once
/* ATOS-specific calls with no direct POSIX equivalent. */
#include <atos/abi.h>

/* Fills *ent with entry `index` of the open directory `fd`. Returns 0, or
 * -1 with errno set (ENOENT once past the last entry). */
int readdir(int fd, unsigned long index, struct atos_dirent *ent);
int fstat(int fd, struct atos_stat *st);

#pragma once
#include <atos/abi.h>

typedef unsigned mode_t;

/* ATOS has no permissions, so `mode` is accepted and ignored. */
int mkdir(const char *path, mode_t mode);

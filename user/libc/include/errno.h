#pragma once
#include <atos/abi.h> /* E* values */

extern int errno;

/* Short description of an errno value. */
const char *strerror(int err);

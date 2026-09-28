#pragma once
#include <unistd.h>

/* Waits for child `pid` (or any child, for -1) to exit. Stores its exit
 * code (not a packed POSIX status word) in *status if non-NULL. */
pid_t waitpid(pid_t pid, int *status, int options);

#pragma once
/* ATOS-specific calls with no direct POSIX equivalent. */
#include <atos/abi.h>

/* Fills *ent with entry `index` of the open directory `fd`. Returns 0, or
 * -1 with errno set (ENOENT once past the last entry). */
int readdir(int fd, unsigned long index, struct atos_dirent *ent);
int fstat(int fd, struct atos_stat *st);

/* The network interface's configuration and counters (-1 + ENODEV if the
 * machine has no NIC). */
int netinfo(struct atos_netinfo *info);

/* One DNS A-record lookup of `name` against `server` (network order)
 * port `port` (host order), waiting at most `timeout_ms`. Returns 0 and
 * the first address in *out, or -1 with errno set (ENOENT: no such name). */
int dns_resolve(const char *name, uint32_t server, uint16_t port, int timeout_ms, uint32_t *out);

/* Milliseconds since boot, at timer-tick (10 ms) resolution. */
unsigned long uptime_ms(void);
/* Sleeps at least `ms` milliseconds (rounded up to whole ticks). */
int msleep(unsigned long ms);

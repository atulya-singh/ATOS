#pragma once
#include <netinet/in.h>

/* Dotted-quad text <-> network-order address. inet_aton returns 1 on
 * success, 0 on malformed input; inet_addr returns INADDR_NONE for that. */
#define INADDR_NONE ((in_addr_t)0xFFFFFFFF)
int inet_aton(const char *text, struct in_addr *out);
in_addr_t inet_addr(const char *text);
/* Returns a static buffer, overwritten by the next call. */
char *inet_ntoa(struct in_addr addr);

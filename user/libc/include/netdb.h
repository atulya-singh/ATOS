#pragma once
#include <netinet/in.h>

struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

/* Resolves a dotted quad or, through DNS (the server from netinfo), a
 * host name to its first IPv4 address. Returns static storage, or NULL
 * with h_errno set. */
struct hostent *gethostbyname(const char *name);
extern int h_errno;
#define HOST_NOT_FOUND 1
#define TRY_AGAIN      2
#define NO_RECOVERY    3

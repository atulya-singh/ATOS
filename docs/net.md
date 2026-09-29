# Networking

Code: `src/kernel/net/`, `src/kernel/dev/virtio_net.c`. Userspace:
`user/libc/net.c` and the programs `ifconfig`, `ping`, `nslookup`,
`wget`, and `httpd`.

## Layers

```
 sockets (socket.c)        fds; datagram queues; blocking with timeouts; syscalls
 TCP (tcp.c)               connections, retransmission, FIN handshake
 UDP, ICMP (ipv4.c)        ports; echo request/reply
 IPv4 (ipv4.c)             header checks, routing (on-link vs gateway)
 ARP (arp.c)               16-entry cache, 60 s TTL, one queued packet per pending entry
 Ethernet (net.c)          framing, padding, dispatch by EtherType
 driver (virtio_net.c)     transmit + poll
```

`inet.c` holds the Internet checksum and address parsing. It is
hardware-free and host-tested against RFC 1071's worked example and a
real IPv4 header.

## Threading and locking

**One mutex.** `net_lock` guards all protocol state. It is a mutex, not
a spinlock, because socket calls block and copy user memory while
holding it.

**The net thread.** A kernel thread named `net`:

1. Polls the NIC.
2. Runs the ARP, TCP, and socket timers once per tick.
3. Paces itself. After traffic, it polls every scheduling round for a
   few ticks to keep latency low during an exchange. When quiet, it
   polls once a tick.

The NIC's interrupt is not used (see [drivers.md](drivers.md)).

**Blocking calls.** A blocking socket call:

1. Records the socket's event counter.
2. Drops `net_lock`.
3. Sleeps with `wait_event` until the counter changes or its deadline
   passes. The net thread wakes sockets whose deadline has passed.
4. Retakes the lock and rechecks its condition.

## Configuration

The address is static. The defaults match QEMU's user-mode network:

| Setting | Default |
|---------|---------|
| Address | `10.0.2.15/24` |
| Gateway | `10.0.2.2` |
| DNS | `10.0.2.3` |

Override them with `ip=`, `netmask=`, `gw=`, and `dns=` on the kernel
command line. There is no DHCP client yet.

## IPv4, ICMP, UDP

- **IPv4.**
  - Incoming packets need valid headers and checksums. Fragments are
    dropped, since there is no reassembly.
  - Outgoing packets set DF, TTL 64, and never carry options.
  - Routing: on-link destinations go direct; everything else goes to the
    gateway.
- **ICMP.** Echo requests are answered in the kernel. Echo replies go to
  the ICMP socket whose identifier matches.
- **UDP.** Checksums are verified when present and always sent. A
  datagram with no bound socket is dropped silently.

## TCP

This is a deliberately small TCP (RFC 793 and the RFC 1122 essentials):

- **Opening.** Active open (`connect`) and passive open (`listen`/`accept`,
  with a backlog of at most 16). The SYN carries an MSS option; the peer's
  MSS is honored.
- **Buffers.** Each connection has a 16 KiB send buffer and a 16 KiB
  receive buffer. The advertised window is the free receive space. A
  window update is sent once the application drains a half-full buffer.
- **Receiving.** Data is accepted in order. Out-of-order segments are
  dropped and re-ACKed, so the peer retransmits. Overlapping
  retransmissions are trimmed.
- **Retransmission.** Go-back-N from the oldest unacknowledged byte. The
  retransmission timeout starts at 1 s and doubles up to 8 s; after 6
  retries the connection is reset with `ETIMEDOUT`. A zero peer window
  gets a one-byte probe.
- **Closing.** The full FIN state machine: `FIN_WAIT_1/2`, `CLOSING`,
  `TIME_WAIT` (2 s), `CLOSE_WAIT`, and `LAST_ACK`. A closed socket
  becomes an orphan connection that finishes the handshake on its own and
  is freed after 30 s at the latest.
- **Resets.** An RST counts only at exactly the expected sequence number
  (RFC 5961's simplest form). A segment for no connection gets an RST.

Not implemented: congestion control, SACK, window scaling, timestamps,
urgent data, and delayed ACKs.

## Sockets API

The kernel side is in [syscalls.md](syscalls.md). libc provides BSD
sockets:

- `socket`, `bind`, `connect`, `listen`, `accept`
- `send`/`recv` and `sendto`/`recvfrom`
- `setsockopt(SOL_SOCKET, SO_RCVTIMEO)`
- `inet_aton`, `inet_ntoa`, and `inet_addr`
- `gethostbyname`, which uses a DNS resolver (`dns_resolve`) over UDP
  with three tries

As on Linux, `socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP)` is a ping
socket: you send echo requests, and the kernel fills in the identifier
and checksum and delivers only the matching replies.

## Tools

| Command | Does |
|---------|------|
| `ifconfig` | Address, MAC, and packet counters |
| `ping [-c n] host` | ICMP echo, one per second, with round-trip times at 10 ms resolution |
| `nslookup name [server[:port]]` | One DNS A query |
| `wget [-O file] http://host[:port]/path` | HTTP/1.0 GET; `-O -` writes to stdout |
| `httpd [-n count] [port]` | Serves the filesystem over HTTP/1.0: files as-is, directories as a listing |

## Trying it

`tools/run.sh` attaches a virtio-net NIC to QEMU's user-mode network and
forwards host port 8080 to the guest's port 80:

```
atos$ ping -c 3 10.0.2.2
atos$ nslookup example.com          # through QEMU's forwarder, if the host has DNS
atos$ wget -O - http://example.com/
atos$ httpd                          # then, on the host: curl localhost:8080/README
```

The smoke test covers all of this against a local perl server
(`tools/net-test-server.pl`): ping, DNS answers and NXDOMAIN, a 200 KB
download checked byte for byte, a 404, and the host fetching a file from
the guest's `httpd`.

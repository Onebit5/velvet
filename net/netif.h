// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/netif.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the interface: the one place that knows there is both a card and a
 * stack, and the only file in net/ that is not pure.
 */

/* the design notes for netif.h are in docs/subsystems/net.rst */

#ifndef NET_NETIF_H
#define NET_NETIF_H

#include "net/net.h"
#include "net/arp.h"
#include "net/dhcp.h"
#include "net/tcp.h"
#include "net/dns.h"

/*
 * the interface: the one place that knows there is both a card and a
 * stack, and the only file in net/ that is not pure.
 *
 * everything else here turns bytes into headers and back. this decides
 * what to do with them, which is a different job, and keeping the two
 * apart is why the protocols can be tested against packets written by
 * hand and this cannot.
 *
 * there is exactly one interface. a machine with two would want a
 * routing table, and the roadmap is explicit that there is no routing
 * here: this machine and whatever else is on the wire.
 */

bool net_up(ipv4 address, ipv4 netmask);
bool net_is_up(void);

ipv4 net_address(void);
ipv4 net_netmask(void);
const struct mac *net_mac(void);

void net_dhcp_start(void);
void net_dhcp_stop(void);
const struct dhcp *net_dhcp(void);

ipv4 net_router(void);
ipv4 net_dns(void);

/*
 * a frame arrived. dispatches by ethertype and then by protocol, and
 * quietly drops anything it does not understand, which on a real wire
 * is most of what arrives
 */
void net_receive(const uint8_t *frame, size_t len);

/* take whatever the card has, and keep taking until there is nothing. */
void net_poll(void);

void net_service_ready(void);

#define NET_FALLBACK_MS 500

bool net_send_ip(ipv4 to, uint8_t protocol, const void *payload, size_t len);

bool net_arp_request(ipv4 who);

const struct arp_cache *net_arp_cache(void);

struct net_stats {
    uint64_t frames_in, frames_out;
    uint64_t arp_in, arp_out;
    uint64_t ip_in, icmp_in, udp_in, tcp_in;
    uint64_t dropped_not_mine, dropped_bad;
};
void net_get_stats(struct net_stats *out);

/*
 * the table lives here rather than in socket.c because a datagram is
 * delivered on the poll thread and taken on somebody else's, and one
 * lock over both is the whole of what makes that safe. socket.c is the
 * pure half: a ring, an owner, and a port
 */

int  net_socket_open(int owner, uint16_t port);
bool net_socket_close(int owner, int handle);
void net_socket_close_all(int owner);
bool net_socket_send(int owner, int handle, ipv4 to, uint16_t port,
                     const void *data, size_t len);
int64_t net_socket_take(int owner, int handle, ipv4 *from,
                        uint16_t *from_port, void *out, size_t max);

int64_t net_socket_wait(int owner, int handle, ipv4 *from,
                        uint16_t *from_port, void *out, size_t max,
                        int64_t timeout_ms);

size_t net_socket_ready(int owner, const int *handles, size_t count,
                        int64_t timeout_ms, bool *ready);

/* handles are offset by TCP_HANDLE_BASE so that one number space serves both kinds. */
#define TCP_HANDLE_BASE 64

int  net_tcp_open(int owner, ipv4 to, uint16_t port);
int  net_tcp_serve(int owner, uint16_t port);
int  net_tcp_accept(int owner, int handle, int64_t timeout_ms);
int64_t net_tcp_send(int owner, int handle, const void *data, size_t len);
int64_t net_tcp_recv(int owner, int handle, void *out, size_t max,
                     int64_t timeout_ms);
bool net_tcp_shut(int owner, int handle);
void net_tcp_close_all(int owner);
int  net_tcp_state(int owner, int handle);

int net_tcp_connect(ipv4 to, uint16_t port);
int net_tcp_listen(uint16_t port);
struct tcp_conn *net_tcp_at(int i);

void net_tcp_forget(int i);

enum dns_result net_resolve(const char *name, ipv4 *out);

void net_dns_stats(uint64_t *asked, uint64_t *answered, uint64_t *cached);

/*
 * icmp echo is the smallest possible round trip, so it is the first
 * thing worth having and the best thing to debug with: when it works the
 * card, arp, ip and the checksum are all working, and when it does not
 * the fault is in one of four things rather than forty.
 *
 * the reply arrives on the poll thread, so a caller sends and then waits
 * for the sequence number to come back rather than being handed one
 */
bool net_ping_send(ipv4 to, uint16_t seq);
bool net_ping_seen(uint16_t seq, uint64_t *when_ms);

#endif

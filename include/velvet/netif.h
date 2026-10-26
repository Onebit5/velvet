// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/netif.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the interface: the one place that knows there is both a card and a
 * stack, and the only file in net/ that is not pure.
 */

/* the design notes for netif.h are in docs/subsystems/net.rst */

#ifndef VELVET_NETIF_H
#define VELVET_NETIF_H

#include <velvet/net.h>
#include <velvet/arp.h>
#include <velvet/tcp.h>

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

/*
 * what this machine is on the wire, and where whatever is not on it goes.
 * a router of 0 is none: only this wire can be reached
 */
bool net_up(ipv4 address, ipv4 netmask, ipv4 router);
bool net_is_up(void);

ipv4 net_address(void);
ipv4 net_netmask(void);
const struct mac *net_mac(void);

ipv4 net_router(void);

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

/*
 * the table lives here rather than in socket.c because a datagram is
 * delivered on the poll thread and taken on somebody else's, and one
 * lock over both is the whole of what makes that safe. socket.c is the
 * pure half: a ring, an owner, and a port
 */

int  net_socket_open(int owner, uint16_t port);
bool net_socket_close(int owner, int handle);
bool net_socket_send(int owner, int handle, ipv4 to, uint16_t port,
                     const void *data, size_t len);
int64_t net_socket_take(int owner, int handle, ipv4 *from,
                        uint16_t *from_port, void *out, size_t max);



/* how many datagrams are waiting on a socket */
size_t net_socket_pending(int i_owner, int i_handle);

/* handles are offset by TCP_HANDLE_BASE so that one number space serves both kinds. */
#define TCP_HANDLE_BASE 64

int  net_tcp_open(int owner, ipv4 to, uint16_t port);
int  net_tcp_serve(int owner, uint16_t port);
int64_t net_tcp_send(int owner, int handle, const void *data, size_t len);
/*
 * what has arrived, up to max: how much, 0 at the end of the stream, or
 * -1 for nothing yet. nothing here waits; a caller that wants to looks
 * again
 */
int64_t net_tcp_recv(int owner, int handle, void *out, size_t max);
bool net_tcp_shut(int owner, int handle);
int  net_tcp_state(int owner, int handle);

/* why a connection ended, as an errno, or 0 while it has not */
int net_tcp_error(int i_owner, int i_handle);

/* a connection handed to another owner, as accept hands one over */
bool net_tcp_give(int i_owner, int i_handle, int i_new_owner);

/* bytes received and not yet read, or -1 for a handle not the owner's */
int64_t net_tcp_waiting(int i_owner, int i_handle);

/* the addresses and ports at either end */
bool net_tcp_ends(int i_owner, int i_handle, ipv4 *p_local,
                  uint16_t *p_local_port, ipv4 *p_remote,
                  uint16_t *p_remote_port);

int net_tcp_connect(ipv4 to, uint16_t port);
int net_tcp_listen(uint16_t port);

#endif

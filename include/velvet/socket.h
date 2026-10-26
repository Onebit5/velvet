// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/socket.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * enough of a socket layer for a program to use.
 */

/* the design notes for socket.h are in docs/subsystems/mm.rst */

#ifndef VELVET_SOCKET_H
#define VELVET_SOCKET_H

#include <velvet/net.h>

#define SOCKET_MAX      8       /* how many may be open at once */
#define SOCKET_QUEUE    8       /* datagrams held per socket */
#define SOCKET_DATAGRAM 1472    /* an mtu, less the ip and udp headers */

struct socket_datagram {
    ipv4     from;
    uint16_t from_port;
    uint16_t len;
    uint8_t  data[SOCKET_DATAGRAM];
};

struct socket {
    bool     used;
    int      owner;             /* the socket file it belongs to */
    uint16_t port;

    struct socket_datagram q[SOCKET_QUEUE];
    size_t   head, count;       /* a ring: head is the oldest waiting */
    uint64_t dropped;           /* arrived with the queue full */
};

struct socket_table {
    struct socket s[SOCKET_MAX];
};

void socket_table_reset(struct socket_table *t);

int socket_open(struct socket_table *t, int owner, uint16_t port);

bool socket_close(struct socket_table *t, int owner, int handle);

/*
 * a datagram arrived. returns *which* socket took it, or -1 when
 * nothing had that port open, which is ordinary and is not an error
 * worth reporting anywhere. a socket whose queue was full is still
 * named: nothing was added, but the datagram did reach a socket
 */
int socket_deliver(struct socket_table *t, uint16_t to_port, ipv4 from,
                   uint16_t from_port, const void *data, size_t len);

int64_t socket_take(struct socket_table *t, int owner, int handle,
                    ipv4 *from, uint16_t *from_port, void *out, size_t max);

bool socket_port(const struct socket_table *t, int owner, int handle,
                 uint16_t *out);

size_t socket_waiting(const struct socket_table *t, int owner, int handle);

#endif

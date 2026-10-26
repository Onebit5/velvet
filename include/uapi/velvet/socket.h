// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/socket.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * sockets: the families, the kinds, and an internet address.
 */

#ifndef UAPI_VELVET_SOCKET_H
#define UAPI_VELVET_SOCKET_H

#include <stdint.h>

#define AF_UNSPEC 0
#define AF_UNIX   1
#define AF_INET   2
#define AF_INET6  10

#define SOCK_STREAM   1
#define SOCK_DGRAM    2
#define SOCK_RAW      3
#define SOCK_TYPE_MASK 0xf
#define SOCK_NONBLOCK 00004000
#define SOCK_CLOEXEC  02000000

#define SOL_SOCKET    1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_BROADCAST  6
#define SO_SNDBUF     7
#define SO_RCVBUF     8
#define SO_KEEPALIVE  9
#define SO_LINGER     13
#define SO_RCVTIMEO   20
#define SO_SNDTIMEO   21

#define IPPROTO_IP   0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

#define TCP_NODELAY 1

#define MSG_OOB      0x0001
#define MSG_PEEK     0x0002
#define MSG_DONTWAIT 0x0040
#define MSG_WAITALL  0x0100
#define MSG_NOSIGNAL 0x4000

#define SHUT_RD   0
#define SHUT_WR   1
#define SHUT_RDWR 2

/* an internet address. the port and the address are in network order */
struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t  sin_zero[8];
};

#define INADDR_ANY       0x00000000u
#define INADDR_BROADCAST 0xffffffffu

struct iovec;

struct msghdr {
    uint64_t msg_name;          /* where it goes or came from, or 0 */
    uint32_t msg_namelen;
    uint32_t pad0;
    uint64_t msg_iov;           /* struct iovec[msg_iovlen] */
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    int32_t  msg_flags;
    uint32_t pad1;
};

#endif

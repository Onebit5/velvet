// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/if.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * an interface, as a socket's ioctls name it and say what it is.
 */

#ifndef UAPI_VELVET_IF_H
#define UAPI_VELVET_IF_H

#include <stdint.h>

#include "socket.h"

#define IFNAMSIZ 16

#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_RUNNING   0x40
#define IFF_MULTICAST 0x1000

#define ARPHRD_ETHER  1         /* what a hardware address is, in sa_family */

struct ifreq {
    char ifr_name[IFNAMSIZ];
    union {
        struct sockaddr ifr_addr;
        struct sockaddr ifr_hwaddr;
        int16_t ifr_flags;
        int32_t ifr_ifindex;
        int32_t ifr_mtu;
        uint8_t ifr_pad[24];
    };
};

_Static_assert(sizeof(struct ifreq) == 40,
               "an ifreq is a name and twenty-four bytes of answer");

#endif

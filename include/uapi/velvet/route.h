// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/route.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a route, as SIOCADDRT and SIOCDELRT are handed one.
 */

#ifndef UAPI_VELVET_ROUTE_H
#define UAPI_VELVET_ROUTE_H

#include <stddef.h>
#include <stdint.h>

#include "socket.h"

#define RTF_UP      0x1
#define RTF_GATEWAY 0x2

/*
 * the fields a program sets are the three addresses and the flags; the
 * rest are the layout every program's headers give it, pointers included,
 * and are read as nothing
 */
struct rtentry {
    uint64_t        rt_pad1;
    struct sockaddr rt_dst;
    struct sockaddr rt_gateway;
    struct sockaddr rt_genmask;
    uint16_t        rt_flags;
    int16_t         rt_pad2;
    uint64_t        rt_pad3;
    uint64_t        rt_pad4;
    int16_t         rt_metric;
    uint64_t        rt_dev;
    uint64_t        rt_mtu;
    uint64_t        rt_window;
    uint16_t        rt_irtt;
};

_Static_assert(sizeof(struct rtentry) == 120
               && offsetof(struct rtentry, rt_flags) == 56,
               "a route is laid out the way every program lays it out");

#endif

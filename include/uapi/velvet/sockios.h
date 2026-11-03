// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/sockios.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a socket is asked about the interface under it and the route
 * through it.
 */

#ifndef UAPI_VELVET_SOCKIOS_H
#define UAPI_VELVET_SOCKIOS_H

#define SIOCADDRT       0x890b  /* a route, as a struct rtentry */
#define SIOCDELRT       0x890c
#define SIOCGIFFLAGS    0x8913  /* the rest take a struct ifreq */
#define SIOCSIFFLAGS    0x8914
#define SIOCGIFADDR     0x8915
#define SIOCSIFADDR     0x8916
#define SIOCGIFBRDADDR  0x8919
#define SIOCGIFNETMASK  0x891b
#define SIOCSIFNETMASK  0x891c
#define SIOCGIFMTU      0x8921
#define SIOCGIFHWADDR   0x8927
#define SIOCGIFINDEX    0x8933

#endif

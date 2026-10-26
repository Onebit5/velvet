// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/poll.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what poll and select wait for.
 */

#ifndef UAPI_VELVET_POLL_H
#define UAPI_VELVET_POLL_H

#include <stdint.h>

struct pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

#define POLLIN     0x0001
#define POLLPRI    0x0002
#define POLLOUT    0x0004
#define POLLERR    0x0008
#define POLLHUP    0x0010
#define POLLNVAL   0x0020
#define POLLRDNORM 0x0040
#define POLLRDBAND 0x0080
#define POLLWRNORM 0x0100
#define POLLWRBAND 0x0200

/* select's sets: a bit per descriptor, this many of them */
#define FD_SETSIZE 1024

#endif

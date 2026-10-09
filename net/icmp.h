// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/icmp.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * icmp, which for this machine is entirely `ping`.
 */

/* the design notes for icmp.h are in docs/subsystems/mm.rst */

#ifndef NET_ICMP_H
#define NET_ICMP_H

#include <velvet/net.h>

#define ICMP_HEADER_LEN 8

#define ICMP_ECHO_REPLY   0
#define ICMP_ECHO_REQUEST 8

struct icmp_echo {
    uint8_t  type;
    uint16_t id;
    uint16_t seq;
    size_t   payload_at;
    size_t   payload_len;
};

bool icmp_parse_echo(const uint8_t *p, size_t len, struct icmp_echo *out);

size_t icmp_build_echo(uint8_t *p, uint8_t type, uint16_t id, uint16_t seq,
                       const void *payload, size_t payload_len);

#endif

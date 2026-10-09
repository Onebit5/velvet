// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/udp.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * udp: two ports, a length, and a checksum nobody agrees about.
 */

/* the design notes for udp.h are in docs/subsystems/mm.rst */

#ifndef NET_UDP_H
#define NET_UDP_H

#include <velvet/net.h>

#define UDP_HEADER_LEN 8

struct udp_header {
    uint16_t from_port;
    uint16_t to_port;
    uint16_t length;            /* header and payload together */
    size_t   payload_at;
    size_t   payload_len;
};

bool udp_parse(const uint8_t *p, size_t len, ipv4 from, ipv4 to,
               struct udp_header *out);

size_t udp_build(uint8_t *p, ipv4 from, ipv4 to,
                 uint16_t from_port, uint16_t to_port,
                 const void *payload, size_t payload_len);

#endif

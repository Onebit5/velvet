// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/ip.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ipv4, and only the parts a machine on one wire needs.
 */

/* the design notes for ip.h are in docs/subsystems/mm.rst */

#ifndef VELVET_IP_H
#define VELVET_IP_H

#include <velvet/net.h>

#define IP_HEADER_MIN 20
#define IP_VERSION 4

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

struct ip_header {
    uint8_t  header_len;        /* in bytes, already multiplied out */
    uint8_t  protocol;
    uint8_t  ttl;
    uint16_t total_len;
    uint16_t id;
    ipv4     from;
    ipv4     to;

    /*
     * where the payload starts and how long it is, worked out here so
     * that no caller has to do the same arithmetic slightly differently
     */
    size_t   payload_at;
    size_t   payload_len;
};

/*
 * pull a header apart, and refuse anything this machine cannot act on
 * correctly: not version 4, a header shorter than the minimum, a total
 * length that disagrees with how much actually arrived, a bad checksum,
 * or a fragment
 */
bool ip_parse(const uint8_t *p, size_t len, struct ip_header *out);

size_t ip_build(uint8_t *p, ipv4 from, ipv4 to, uint8_t protocol,
                uint16_t payload_len, uint16_t id);

bool ip_same_network(ipv4 a, ipv4 b, ipv4 netmask);

ipv4 ip_next_hop(ipv4 to, ipv4 me, ipv4 netmask, ipv4 gateway);

#endif

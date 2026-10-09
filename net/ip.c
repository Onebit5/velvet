// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/ip.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ipv4, and the checksum that covers its header.
 */

#include <velvet/ip.h>

bool ip_parse(const uint8_t *p, size_t len, struct ip_header *out)
{
    if (len < IP_HEADER_MIN) {
        return false;
    }
    if ((p[0] >> 4) != IP_VERSION) {
        return false;
    }

    uint8_t ihl = (uint8_t)((p[0] & 0x0f) * 4);
    if (ihl < IP_HEADER_MIN || ihl > len) {
        return false;       /* options that run off the end of what arrived */
    }

    uint16_t total = get16be(p + 2);
    if (total < ihl || total > len) {
        /* the field disagrees with the frame. */
        return false;
    }

    /*
     * the checksum covers the header only, and includes its own field,
     * so summing the lot gives zero when it is right
     */
    if (net_checksum(p, ihl) != 0) {
        return false;
    }

    /*
     * fragments. bit 13 of the flags/offset word is "more fragments",
     * and a non-zero offset means this is not the first piece. either
     * way it is part of a datagram and not a datagram, and this machine
     * does not reassemble, so it is dropped rather than acted on
     */
    uint16_t frag = get16be(p + 6);
    if ((frag & 0x2000) != 0 || (frag & 0x1fff) != 0) {
        return false;
    }

    out->header_len = ihl;
    out->total_len  = total;
    out->id         = get16be(p + 4);
    out->ttl        = p[8];
    out->protocol   = p[9];
    out->from       = get32be(p + 12);
    out->to         = get32be(p + 16);
    out->payload_at  = ihl;
    out->payload_len = (size_t)(total - ihl);
    return true;
}

size_t ip_build(uint8_t *p, ipv4 from, ipv4 to, uint8_t protocol,
                uint16_t payload_len, uint16_t id)
{
    p[0] = (IP_VERSION << 4) | (IP_HEADER_MIN / 4);
    p[1] = 0;                                   /* no differentiated services */
    put16be(p + 2, (uint16_t)(IP_HEADER_MIN + payload_len));
    put16be(p + 4, id);

    /* don't fragment, offset zero. */
    put16be(p + 6, 0x4000);

    p[8] = 64;                                  /* the conventional ttl */
    p[9] = protocol;
    put16be(p + 10, 0);                         /* the checksum, once it is
                                                 * known, it has to be zero
                                                 * while it is computed */
    put32be(p + 12, from);
    put32be(p + 16, to);

    put16be(p + 10, net_checksum(p, IP_HEADER_MIN));
    return IP_HEADER_MIN;
}

bool ip_same_network(ipv4 a, ipv4 b, ipv4 netmask)
{
    return (a & netmask) == (b & netmask);
}

ipv4 ip_next_hop(ipv4 to, ipv4 me, ipv4 netmask, ipv4 gateway)
{
    if (ip_same_network(to, me, netmask)) {
        return to;
    }

    /* the broadcast address is never routed. */
    if (to == 0xffffffff) {
        return 0;
    }
    return gateway;             /* 0 when there is none, which is the
                                 * honest answer rather than a guess */
}

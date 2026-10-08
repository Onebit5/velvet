// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/udp.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * udp: two ports, a length, and a checksum.
 */

#include "net/udp.h"
#include "net/ip.h"
#include "lib/string.h"

/* the pseudo-header, which is twelve bytes that are never sent. */
static void sum_pseudo(struct net_sum *s, ipv4 from, ipv4 to, uint16_t len)
{
    uint8_t p[12];
    put32be(p,     from);
    put32be(p + 4, to);
    p[8] = 0;
    p[9] = IP_PROTO_UDP;
    put16be(p + 10, len);
    net_sum_add(s, p, sizeof p);
}

bool udp_parse(const uint8_t *p, size_t len, ipv4 from, ipv4 to,
               struct udp_header *out)
{
    if (len < UDP_HEADER_LEN) {
        return false;
    }

    uint16_t length = get16be(p + 4);
    if (length < UDP_HEADER_LEN || length > len) {
        /* the field disagrees with the datagram. */
        return false;
    }

    /* zero means the sender did not compute one. */
    if (get16be(p + 6) != 0) {
        struct net_sum s;
        net_sum_start(&s);
        sum_pseudo(&s, from, to, length);
        net_sum_add(&s, p, length);
        if (net_sum_finish(&s) != 0) {
            return false;
        }
    }

    out->from_port   = get16be(p);
    out->to_port     = get16be(p + 2);
    out->length      = length;
    out->payload_at  = UDP_HEADER_LEN;
    out->payload_len = (size_t)(length - UDP_HEADER_LEN);
    return true;
}

size_t udp_build(uint8_t *p, ipv4 from, ipv4 to,
                 uint16_t from_port, uint16_t to_port,
                 const void *payload, size_t payload_len)
{
    uint16_t length = (uint16_t)(UDP_HEADER_LEN + payload_len);

    put16be(p,     from_port);
    put16be(p + 2, to_port);
    put16be(p + 4, length);
    put16be(p + 6, 0);          /* zero while the sum is taken over it */

    if (payload_len > 0) {
        memcpy(p + UDP_HEADER_LEN, payload, payload_len);
    }

    struct net_sum s;
    net_sum_start(&s);
    sum_pseudo(&s, from, to, length);
    net_sum_add(&s, p, length);

    uint16_t sum = net_sum_finish(&s);

    /*
     * a computed zero is written as all-ones instead, because zero on
     * the wire means "not computed". the two are the same number in
     * ones-complement arithmetic, which is the only reason this trick
     * works at all
     */
    put16be(p + 6, sum == 0 ? 0xffff : sum);
    return length;
}

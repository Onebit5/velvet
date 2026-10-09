// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/icmp.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * icmp, which for this machine is entirely ping.
 */

#include "icmp.h"
#include <velvet/string.h>

bool icmp_parse_echo(const uint8_t *p, size_t len, struct icmp_echo *out)
{
    if (len < ICMP_HEADER_LEN) {
        return false;
    }
    if (p[0] != ICMP_ECHO_REQUEST && p[0] != ICMP_ECHO_REPLY) {
        return false;
    }
    if (p[1] != 0) {
        return false;           /* no echo has a code */
    }

    /*
     * over the header and the payload together, and it includes its own
     * field, so the whole thing sums to zero when it is right
     */
    if (net_checksum(p, len) != 0) {
        return false;
    }

    out->type = p[0];
    out->id   = get16be(p + 4);
    out->seq  = get16be(p + 6);
    out->payload_at  = ICMP_HEADER_LEN;
    out->payload_len = len - ICMP_HEADER_LEN;
    return true;
}

size_t icmp_build_echo(uint8_t *p, uint8_t type, uint16_t id, uint16_t seq,
                       const void *payload, size_t payload_len)
{
    p[0] = type;
    p[1] = 0;
    put16be(p + 2, 0);          /* zero while the sum is taken over it */
    put16be(p + 4, id);
    put16be(p + 6, seq);

    if (payload_len > 0) {
        memcpy(p + ICMP_HEADER_LEN, payload, payload_len);
    }

    size_t total = ICMP_HEADER_LEN + payload_len;
    put16be(p + 2, net_checksum(p, total));
    return total;
}

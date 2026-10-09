// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/net.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the wire: framing, addresses, and byte order.
 */

#include <velvet/net.h>
#include <velvet/string.h>

const struct mac MAC_BROADCAST = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
const struct mac MAC_ZERO      = { { 0, 0, 0, 0, 0, 0 } };

bool mac_equal(const struct mac *a, const struct mac *b)
{
    return memcmp(a->b, b->b, MAC_LEN) == 0;
}

bool mac_is_broadcast(const struct mac *m)
{
    return mac_equal(m, &MAC_BROADCAST);
}

static char hex(unsigned v)
{
    return (char)(v < 10 ? '0' + v : 'a' + (v - 10));
}

void mac_format(const struct mac *m, char *out, size_t max)
{
    /* 17 characters and a terminator. */
    if (max < 18) {
        if (max > 0) {
            out[0] = '\0';
        }
        return;
    }
    size_t n = 0;
    for (int i = 0; i < MAC_LEN; i++) {
        if (i > 0) {
            out[n++] = ':';
        }
        out[n++] = hex((unsigned)m->b[i] >> 4);
        out[n++] = hex((unsigned)m->b[i] & 0xf);
    }
    out[n] = '\0';
}

void ipv4_format(ipv4 addr, char *out, size_t max)
{
    if (max < 16) {
        if (max > 0) {
            out[0] = '\0';
        }
        return;
    }
    size_t n = 0;
    for (int shift = 24; shift >= 0; shift -= 8) {
        unsigned byte = (addr >> shift) & 0xff;
        if (shift != 24) {
            out[n++] = '.';
        }
        if (byte >= 100) {
            out[n++] = (char)('0' + byte / 100);
        }
        if (byte >= 10) {
            out[n++] = (char)('0' + (byte / 10) % 10);
        }
        out[n++] = (char)('0' + byte % 10);
    }
    out[n] = '\0';
}

bool ipv4_parse(const char *s, ipv4 *out)
{
    ipv4 addr = 0;

    for (int part = 0; part < 4; part++) {
        if (part > 0) {
            if (*s != '.') {
                return false;
            }
            s++;
        }
        if (*s < '0' || *s > '9') {
            return false;       /* an empty part is not a zero */
        }

        unsigned v = 0, digits = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (unsigned)(*s - '0');
            s++;
            if (++digits > 3 || v > 255) {
                return false;
            }
        }
        addr = (addr << 8) | v;
    }

    if (*s != '\0') {
        return false;           /* trailing rubbish is not an address */
    }
    *out = addr;
    return true;
}

/*
 * a byte at a time, which is slower than the usual word-at-a-time
 * version and is correct for a case that one gets wrong: a piece of odd
 * length followed by another piece.
 *
 * the sum is over 16-bit big-endian words. if a chunk ends half way
 * through a word, the next chunk's first byte is that word's *low* half
 *, so the parity has to be carried between calls. a word-at-a-time
 * implementation that restarts alignment on every chunk gets the right
 * answer for every packet whose pieces all happen to be even, which is
 * most of them, and the wrong answer for udp with an odd payload.
 */

void net_sum_start(struct net_sum *s)
{
    s->acc = 0;
    s->odd = false;
}

void net_sum_add(struct net_sum *s, const void *data, size_t len)
{
    const uint8_t *p = data;

    for (size_t i = 0; i < len; i++) {
        if (!s->odd) {
            s->acc += (uint32_t)p[i] << 8;      /* the high half */
        } else {
            s->acc += p[i];
        }
        s->odd = !s->odd;
    }
}

uint16_t net_sum_finish(const struct net_sum *s)
{
    uint32_t acc = s->acc;

    /* fold the carries back in. */
    while (acc >> 16) {
        acc = (acc & 0xffff) + (acc >> 16);
    }
    return (uint16_t)~acc;
}

uint16_t net_checksum(const void *data, size_t len)
{
    struct net_sum s;
    net_sum_start(&s);
    net_sum_add(&s, data, len);
    return net_sum_finish(&s);
}

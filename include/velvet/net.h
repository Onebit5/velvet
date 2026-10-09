// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/net.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the wire.
 */

/* the design notes for net.h are in docs/subsystems/mm.rst */

#ifndef VELVET_NET_H
#define VELVET_NET_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * the wire. everything in this directory is about one idea: a network
 * protocol is almost entirely *arithmetic over a byte buffer*. a header is
 * a set of offsets, a checksum is a sum, an address is four bytes, none of
 * it needs a card and almost none of it needs a kernel. so the card is one
 * file and the rest is pure, which means every protocol here can be tested
 * against packets written by hand and against real captured ones.
 *
 * the wire is big-endian and this machine is not, so every multi-byte field
 * in every header has to be turned around, and forgetting one is the most
 * common bug in the subject: it produces a packet that looks almost right
 * and is silently discarded by everything that receives it.
 *
 * so there are no structs overlaid on packets, only explicit get and put
 * functions at explicit offsets, the same way fs/ext4.c reads a superblock:
 * the offsets *are* the format, and a struct is a claim about padding and
 * order that the compiler is free to reinterpret
 */

#define MAC_LEN 6

struct mac {
    uint8_t b[MAC_LEN];
};

typedef uint32_t ipv4;

#define IPV4(a, b, c, d) \
    (((ipv4)(a) << 24) | ((ipv4)(b) << 16) | ((ipv4)(c) << 8) | (ipv4)(d))

extern const struct mac MAC_BROADCAST;
extern const struct mac MAC_ZERO;

bool mac_equal(const struct mac *a, const struct mac *b);
bool mac_is_broadcast(const struct mac *m);

/*
 * both of these write into a caller's buffer rather than returning a
 * pointer into a static, because two addresses in one kprintf is a
 * perfectly ordinary thing to want
 */
void mac_format(const struct mac *m, char *out, size_t max);
void ipv4_format(ipv4 addr, char *out, size_t max);

bool ipv4_parse(const char *s, ipv4 *out);

static inline uint16_t get16be(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] << 8 | p[1]);
}

static inline uint32_t get32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16
         | (uint32_t)p[2] << 8  | (uint32_t)p[3];
}

static inline void put16be(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/*
 * one algorithm, used by ip, icmp and udp, and it is stranger than it
 * looks: the ones-complement sum of the data taken as 16-bit big-endian
 * words, complemented.
 *
 * two properties make it worth understanding rather than copying. it is
 * *endian-neutral*, summing the bytes in the wrong order gives the
 * same answer byte-swapped, which is why so many implementations are
 * accidentally correct. and summing a block that already contains its
 * own checksum gives zero, which is how a receiver checks one without
 * having to know where the field was.
 *
 * it is accumulated rather than computed in one call because udp needs a
 * *pseudo-header*, addresses that are not in the datagram at all, to
 * be summed together with the datagram. one call per piece, and the
 * pieces need not be even lengths, which is the part that is usually
 * wrong.
 */

struct net_sum {
    uint32_t acc;
    bool     odd;       /* a byte is waiting, and it is a *high* byte */
};

void net_sum_start(struct net_sum *s);
void net_sum_add(struct net_sum *s, const void *data, size_t len);

uint16_t net_sum_finish(const struct net_sum *s);

uint16_t net_checksum(const void *data, size_t len);

#endif

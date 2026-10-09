// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/arp.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * arp: the question "who has this address, and where do the kernel sends to reach
 * them", asked by shouting it at everybody.
 */

/* the design notes for arp.h are in docs/subsystems/mm.rst */

#ifndef VELVET_ARP_H
#define VELVET_ARP_H

#include <velvet/net.h>

/*
 * arp: the question "who has this address, and where do the kernel sends to reach
 * them", asked by shouting it at everybody.
 *
 * it is the oldest thing on the wire that is still exactly as it was,
 * and it is worth seeing why it has to exist at all. ip addresses are
 * something people assign; ethernet addresses are burned into cards.
 * nothing connects the two except asking. so a machine that wants to
 * send to 10.0.2.2 broadcasts "who has 10.0.2.2?", and whoever does
 * answers with their own hardware address.
 *
 * the answer is then *remembered*, because doing that for every packet
 * would double the traffic on the wire. that cache is the whole of the
 * state in this file, and it is also the whole of the danger: nothing
 * authenticates a reply, so anybody on the wire can claim any address.
 * that is not a flaw the kernel is going to fix here, it is how arp works
 * everywhere, but it is worth writing down rather than discovering.
 */

#define ARP_PACKET_LEN 28

#define ARP_REQUEST 1
#define ARP_REPLY   2

struct arp_packet {
    uint16_t   op;
    struct mac sender_mac;
    ipv4       sender_ip;
    struct mac target_mac;
    ipv4       target_ip;
};

bool arp_parse(const uint8_t *p, size_t len, struct arp_packet *out);

size_t arp_build(uint8_t *p, uint16_t op,
                 const struct mac *sender_mac, ipv4 sender_ip,
                 const struct mac *target_mac, ipv4 target_ip);

#define ARP_CACHE_MAX 16

#define ARP_TTL_MS (2 * 60 * 1000)

struct arp_entry {
    ipv4       ip;
    struct mac mac;
    uint64_t   learned_ms;
    bool       used;
};

struct arp_cache {
    struct arp_entry e[ARP_CACHE_MAX];
};

void arp_cache_reset(struct arp_cache *c);

void arp_learn(struct arp_cache *c, ipv4 ip, const struct mac *mac,
               uint64_t now_ms);

/*
 * look one up. false if it was never learned or has gone stale, and the
 * caller's next move is the same either way: ask
 */
bool arp_lookup(const struct arp_cache *c, ipv4 ip, uint64_t now_ms,
                struct mac *out);

size_t arp_count(const struct arp_cache *c, uint64_t now_ms);
bool arp_at(const struct arp_cache *c, size_t index, uint64_t now_ms,
            struct arp_entry *out);

#endif

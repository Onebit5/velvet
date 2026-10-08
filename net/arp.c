// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/arp.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * arp: asking who owns an address.
 */

#include "net/arp.h"
#include "net/ether.h"
#include "lib/string.h"

/*
 * the fixed preamble every arp packet on ethernet-and-ipv4 begins with:
 * hardware type 1, protocol type 0x0800, six-byte addresses, four-byte
 * addresses. anything else is legal arp for some other wire and is not
 * something this machine can answer
 */
#define ARP_HW_ETHER 1
#define ARP_HW_LEN   6
#define ARP_PROTO_LEN 4

bool arp_parse(const uint8_t *p, size_t len, struct arp_packet *out)
{
    if (len < ARP_PACKET_LEN) {
        return false;
    }
    if (get16be(p) != ARP_HW_ETHER || get16be(p + 2) != ETHERTYPE_IPV4) {
        return false;
    }
    if (p[4] != ARP_HW_LEN || p[5] != ARP_PROTO_LEN) {
        return false;
    }

    out->op = get16be(p + 6);
    memcpy(out->sender_mac.b, p + 8, MAC_LEN);
    out->sender_ip = get32be(p + 14);
    memcpy(out->target_mac.b, p + 18, MAC_LEN);
    out->target_ip = get32be(p + 24);
    return true;
}

size_t arp_build(uint8_t *p, uint16_t op,
                 const struct mac *sender_mac, ipv4 sender_ip,
                 const struct mac *target_mac, ipv4 target_ip)
{
    put16be(p,     ARP_HW_ETHER);
    put16be(p + 2, ETHERTYPE_IPV4);
    p[4] = ARP_HW_LEN;
    p[5] = ARP_PROTO_LEN;
    put16be(p + 6, op);

    memcpy(p + 8, sender_mac->b, MAC_LEN);
    put32be(p + 14, sender_ip);
    memcpy(p + 18, target_mac->b, MAC_LEN);
    put32be(p + 24, target_ip);

    return ARP_PACKET_LEN;
}



void arp_cache_reset(struct arp_cache *c)
{
    memset(c, 0, sizeof *c);
}

static bool fresh(const struct arp_entry *e, uint64_t now_ms)
{
    if (!e->used) {
        return false;
    }
    /*
     * now going backwards would mean the clock was reset under the project, and
     * believing an entry forever is worse than asking again
     */
    if (now_ms < e->learned_ms) {
        return false;
    }
    return now_ms - e->learned_ms < ARP_TTL_MS;
}

void arp_learn(struct arp_cache *c, ipv4 ip, const struct mac *mac,
               uint64_t now_ms)
{
    struct arp_entry *slot = NULL;
    struct arp_entry *oldest = &c->e[0];

    for (size_t i = 0; i < ARP_CACHE_MAX; i++) {
        struct arp_entry *e = &c->e[i];

        if (e->used && e->ip == ip) {
            slot = e;           /* the newest claim wins, which is arp */
            break;
        }
        if (!e->used && slot == NULL) {
            slot = e;
        }
        if (e->learned_ms < oldest->learned_ms) {
            oldest = e;
        }
    }
    if (slot == NULL) {
        slot = oldest;          /* full: the stalest answer goes */
    }

    slot->ip = ip;
    slot->mac = *mac;
    slot->learned_ms = now_ms;
    slot->used = true;
}

bool arp_lookup(const struct arp_cache *c, ipv4 ip, uint64_t now_ms,
                struct mac *out)
{
    for (size_t i = 0; i < ARP_CACHE_MAX; i++) {
        const struct arp_entry *e = &c->e[i];
        if (e->used && e->ip == ip && fresh(e, now_ms)) {
            *out = e->mac;
            return true;
        }
    }
    return false;
}

size_t arp_count(const struct arp_cache *c, uint64_t now_ms)
{
    size_t n = 0;
    for (size_t i = 0; i < ARP_CACHE_MAX; i++) {
        if (fresh(&c->e[i], now_ms)) {
            n++;
        }
    }
    return n;
}

bool arp_at(const struct arp_cache *c, size_t index, uint64_t now_ms,
            struct arp_entry *out)
{
    size_t seen = 0;
    for (size_t i = 0; i < ARP_CACHE_MAX; i++) {
        if (!fresh(&c->e[i], now_ms)) {
            continue;
        }
        if (seen++ == index) {
            *out = c->e[i];
            return true;
        }
    }
    return false;
}

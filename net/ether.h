// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/ether.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ethernet, which is fourteen bytes and one idea.
 */

/* the design notes for ether.h are in docs/subsystems/mm.rst */

#ifndef NET_ETHER_H
#define NET_ETHER_H

#include "net/net.h"

#define ETHER_HEADER_LEN 14
#define ETHER_MIN_FRAME  60     /* without the crc the card adds */
#define ETHER_MTU        1500

#define ETHERTYPE_IPV4 0x0800
#define ETHERTYPE_ARP  0x0806

struct ether_header {
    struct mac to;
    struct mac from;
    uint16_t   type;
};

bool ether_parse(const uint8_t *frame, size_t len, struct ether_header *out);

size_t ether_build(uint8_t *frame, const struct mac *to,
                   const struct mac *from, uint16_t type);

#endif

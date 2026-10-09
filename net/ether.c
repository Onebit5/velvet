// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/ether.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * ethernet: fourteen bytes and one idea.
 */

#include <velvet/ether.h>
#include <velvet/string.h>

bool ether_parse(const uint8_t *frame, size_t len, struct ether_header *out)
{
    if (len < ETHER_HEADER_LEN) {
        return false;
    }
    memcpy(out->to.b,   frame,     MAC_LEN);
    memcpy(out->from.b, frame + 6, MAC_LEN);
    out->type = get16be(frame + 12);
    return true;
}

size_t ether_build(uint8_t *frame, const struct mac *to,
                   const struct mac *from, uint16_t type)
{
    memcpy(frame,     to->b,   MAC_LEN);
    memcpy(frame + 6, from->b, MAC_LEN);
    put16be(frame + 12, type);
    return ETHER_HEADER_LEN;
}

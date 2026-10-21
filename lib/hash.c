// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/hash.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * crc32, the one sum the kernel has any use for.
 */

#include <velvet/hash.h>

/*
 * a nibble at a time out of a sixteen-entry table, which is a tenth the
 * memory of the usual 256-entry one and no slower than anything here
 * needs. this used to live in the gpt driver, privately, because that
 * was the first thing to want it, and a checksum with one caller is a
 * checksum that gets written a second time the moment there are two.
 */

static const uint32_t crc_nibble[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
};

/*
 * the running value is kept *inverted*, which is the part that makes a
 * streaming crc work at all: the algorithm starts at 0xffffffff and ends
 * by complementing, so a caller who wants to carry on from a finished
 * answer has to undo the ending before redoing the middle
 */
uint32_t crc32_more(uint32_t so_far, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = ~so_far;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        crc = (crc >> 4) ^ crc_nibble[crc & 0x0f];
        crc = (crc >> 4) ^ crc_nibble[crc & 0x0f];
    }
    return ~crc;
}

uint32_t crc32_of(const void *data, size_t len)
{
    return crc32_more(crc32_start(), data, len);
}

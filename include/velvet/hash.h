// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/hash.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the two sums the kernel has any use for.
 */

/* the design notes for hash.h are in docs/subsystems/mm.rst */

#ifndef VELVET_HASH_H
#define VELVET_HASH_H

#include <stddef.h>
#include <stdint.h>

/* two sums, lumped together as hashes, that answer different questions. */

struct sha1 {
    uint32_t h[5];
    uint64_t bytes;         /* how much has gone in, for the padding */
    uint8_t  block[64];     /* the part of a block that has arrived */
    size_t   have;
};

void sha1_init(struct sha1 *s);
void sha1_update(struct sha1 *s, const void *data, size_t len);
void sha1_final(struct sha1 *s, uint8_t out[20]);

void sha1_of(const void *data, size_t len, uint8_t out[20]);

void sha1_hex(const uint8_t digest[20], char out[41]);

#define crc32_start() 0u
uint32_t crc32_more(uint32_t so_far, const void *data, size_t len);
uint32_t crc32_of(const void *data, size_t len);

#endif

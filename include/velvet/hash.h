// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/hash.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * crc32, the one sum the kernel has any use for.
 */

/* the design notes for hash.h are in docs/subsystems/mm.rst */

#ifndef VELVET_HASH_H
#define VELVET_HASH_H

#include <stddef.h>
#include <stdint.h>

#define crc32_start() 0u
uint32_t crc32_more(uint32_t so_far, const void *data, size_t len);
uint32_t crc32_of(const void *data, size_t len);

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/chacha20.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the chacha20 block function, which the random generator is built on.
 */

#ifndef VELVET_CHACHA20_H
#define VELVET_CHACHA20_H

#include <stdint.h>

#define CHACHA20_KEY_WORDS 8
#define CHACHA20_BLOCK     64

/*
 * one block of keystream: 64 bytes from a 256 bit key, a 64 bit block
 * counter and a 64 bit nonce, the way the cipher was first described
 */
void chacha20_block(const uint32_t arr_key[CHACHA20_KEY_WORDS],
                    uint64_t u_counter, uint64_t u_nonce,
                    uint8_t arr_out[CHACHA20_BLOCK]);

#endif

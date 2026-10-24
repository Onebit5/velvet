// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/random.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * bytes nobody can predict, for /dev/urandom and whatever else asks.
 */

#ifndef VELVET_RANDOM_H
#define VELVET_RANDOM_H

#include <stddef.h>

/* seeded from what the machine offers: once, before anything asks */
void random_init(void);

/* fill p_buf with sz_len bytes nobody can predict */
void random_bytes(void *p_buf, size_t sz_len);

/*
 * stir something in: bytes a program wrote to /dev/random, or anything
 * else that varies. it can only ever add to what is unpredictable
 */
void random_mix(const void *p_buf, size_t sz_len);

/* the timer's tick, folding in the cycle count it arrived at */
void random_tick(void);

#endif

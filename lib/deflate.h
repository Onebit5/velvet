// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/deflate.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * deflate, and the undoing of it.
 */

/* the design notes for deflate.h are in docs/subsystems/mm.rst */

#ifndef LIB_DEFLATE_H
#define LIB_DEFLATE_H

#include <stddef.h>
#include <stdint.h>

/* deflate, and the undoing of it. */

/*
 * both return the bytes produced, or a negative error, and the caller
 * says how much room there is: a decompressor that cannot be told when
 * to stop writes past the end of a buffer whenever somebody hands it a
 * hostile file
 */

#define INFLATE_BAD_INPUT   (-1)    /* the stream is not a stream */
#define INFLATE_NO_ROOM     (-2)    /* it decompresses to more than was offered */
#define INFLATE_BAD_CHECK   (-3)    /* it unpacked, and to the wrong thing */

long inflate(const void *in, size_t in_len, void *out, size_t out_cap);

long zlib_inflate(const void *in, size_t in_len, void *out, size_t out_cap);

/*
 * fixed huffman with a greedy match search, which is the middle of the
 * three answers: smaller than storing and a great deal less code than
 * building a table per block. a block that would come out larger than it
 * went in is stored instead, so the output is never worse than the
 * input by more than five bytes a block.
 */

long deflate(const void *in, size_t in_len, void *out, size_t out_cap);
long zlib_deflate(const void *in, size_t in_len, void *out, size_t out_cap);

#endif

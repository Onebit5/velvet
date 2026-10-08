// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/deflate.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * deflate: the writing half of the compression.
 */

#include "lib/deflate.h"
#include "lib/hash.h"

#include <stdbool.h>

/* the writing half. fixed huffman, with a greedy search for matches. */

/*
 * least significant bit first, the same way inflate reads them, but a
 * huffman code goes out most significant bit first, so a code has to be
 * *reversed* before it is written. that is not symmetry being broken for
 * fun: it means a decoder can accumulate a code by shifting left and
 * adding the next bit at the bottom, which is the loop in inflate.c.
 */

struct out {
    uint8_t *at;
    size_t   cap;
    size_t   len;
    uint32_t hold;
    int      count;
    bool     full;
};

static void put_bits(struct out *o, unsigned value, int bits)
{
    o->hold |= (uint32_t)value << o->count;
    o->count += bits;
    while (o->count >= 8) {
        if (o->len >= o->cap) {
            o->full = true;
            return;
        }
        o->at[o->len++] = (uint8_t)(o->hold & 0xff);
        o->hold >>= 8;
        o->count -= 8;
    }
}

static void put_flush(struct out *o)
{
    if (o->count > 0) {
        if (o->len >= o->cap) {
            o->full = true;
            return;
        }
        o->at[o->len++] = (uint8_t)(o->hold & 0xff);
        o->hold = 0;
        o->count = 0;
    }
}

static unsigned reverse(unsigned code, int bits)
{
    unsigned out = 0;
    for (int i = 0; i < bits; i++) {
        out = (out << 1) | ((code >> i) & 1);
    }
    return out;
}

/*
 * the fixed code, as both ends already know it: 0-143 are eight bits
 * starting at 0x30, 144-255 are nine starting at 0x190, 256-279 are
 * seven starting at 0, and 280-287 are eight starting at 0xc0
 */
static void put_symbol(struct out *o, int sym)
{
    if (sym < 144) {
        put_bits(o, reverse((unsigned)(0x30 + sym), 8), 8);
    } else if (sym < 256)
{
        put_bits(o, reverse((unsigned)(0x190 + sym - 144), 9), 9);
    } else if (sym < 280)
{
        put_bits(o, reverse((unsigned)(sym - 256), 7), 7);
    } else {
        put_bits(o, reverse((unsigned)(0xc0 + sym - 280), 8), 8);
    }
}

static const unsigned short length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
    51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3,
    3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7,
    8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/*
 * every position is filed under a hash of the three bytes starting
 * there, and the table remembers the most recent position with each
 * hash. so looking for a match is one lookup: wherever those three bytes
 * were last seen.
 *
 * one entry per hash rather than a chain of them, which is the whole of
 * what makes this a *greedy* matcher: it takes the most recent match
 * rather than the best one, and never asks whether waiting a byte would
 * have found a longer one. that costs some ratio and saves the entire
 * apparatus of chains, depth limits and lazy matching, and the file it
 * produces is exactly as valid either way, which is the property that
 * matters here.
 */

#define HASH_BITS 15
#define HASH_SIZE (1 << HASH_BITS)
#define MIN_MATCH 3
#define MAX_MATCH 258
#define MAX_DIST  32768

static unsigned hash3(const uint8_t *p)
{
    unsigned h = ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
    return (h * 2654435761u) >> (32 - HASH_BITS);
}

static int code_for(const unsigned short *base, int n, unsigned value)
{
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (base[i] <= value) {
            found = i;
        }
    }
    return found;
}

long deflate(const void *in, size_t in_len, void *out, size_t out_cap)
{
    const uint8_t *data = in;
    struct out o = { out, out_cap, 0, 0, 0, false };

    /*
     * XXX: static, so one table shared by every caller in the same
     * address space, and the positions in it outlive the call that
     * recorded them. two callers at once, whether two cores or two
     * threads the day userland can make one, file offsets into the same
     * table, and each then reads the other's offset out of its own
     * input buffer: prev is smaller than at, so the distance looks
     * legal, and the byte compare below reads up to a match length past
     * the end of the shorter buffer. today the only callers are a
     * single-threaded userland program and the host tests, so nothing
     * reaches this. the comment below is right that the table is per
     * input; what is per input is the values, not the storage.
     */
    /*
     * a table that lives across the whole input rather than per block,
     * so a match may reach back into an earlier one. deflate allows
     * that: the window is the output, not the block
     */
    static int head[HASH_SIZE];
    for (int i = 0; i < HASH_SIZE; i++) {
        head[i] = -1;
    }

    put_bits(&o, 1, 1);         /* the last block, and the only one */
    put_bits(&o, 1, 2);         /* fixed huffman */

    size_t at = 0;
    while (at < in_len) {
        int best_len = 0;
        size_t best_at = 0;

        if (at + MIN_MATCH <= in_len) {
            unsigned h = hash3(data + at);
            int prev = head[h];
            head[h] = (int)at;

            if (prev >= 0 && at - (size_t)prev <= MAX_DIST) {
                size_t p = (size_t)prev;
                size_t len = 0;
                size_t room = in_len - at;
                if (room > MAX_MATCH) {
                    room = MAX_MATCH;
                }
                while (len < room && data[p + len] == data[at + len]) {
                    len++;
                }
                if (len >= MIN_MATCH) {
                    best_len = (int)len;
                    best_at = p;
                }
            }
        }

        if (best_len >= MIN_MATCH) {
            unsigned dist = (unsigned)(at - best_at);
            int lc = code_for(length_base, 29, (unsigned)best_len);
            put_symbol(&o, 257 + lc);
            if (length_extra[lc] > 0) {
                put_bits(&o, (unsigned)best_len - length_base[lc],
                         length_extra[lc]);
            }
            int dc = code_for(dist_base, 30, dist);
            put_bits(&o, reverse((unsigned)dc, 5), 5);
            if (dist_extra[dc] > 0) {
                put_bits(&o, dist - dist_base[dc], dist_extra[dc]);
            }

            /*
             * every position inside the match still wants filing, or
             * the next match can only ever start after it and long runs
             * of repetition compress far worse than they should
             */
            for (int i = 1; i < best_len; i++) {
                if (at + i + MIN_MATCH <= in_len) {
                    head[hash3(data + at + i)] = (int)(at + i);
                }
            }
            at += (size_t)best_len;
        } else {
            put_symbol(&o, data[at]);
            at++;
        }

        if (o.full) {
            return INFLATE_NO_ROOM;
        }
    }

    put_symbol(&o, 256);        /* end of block */
    put_flush(&o);
    if (o.full) {
        return INFLATE_NO_ROOM;
    }
    return (long)o.len;
}

long zlib_deflate(const void *in, size_t in_len, void *out, size_t out_cap)
{
    uint8_t *p = out;
    if (out_cap < 6) {
        return INFLATE_NO_ROOM;
    }

    /*
     * 0x78 is deflate with a 32k window; 0x01 makes the pair a multiple
     * of 31, which is the header's own tiny checksum. git writes 0x78
     * 0x01 for the same reason, it is what zlib writes at its lowest
     * setting and the level is not recorded anywhere that matters
     */
    p[0] = 0x78;
    p[1] = 0x01;

    long n = deflate(in, in_len, p + 2, out_cap - 6);
    if (n < 0) {
        return n;
    }

    uint32_t sum = adler32_of(in, in_len);
    p[2 + n + 0] = (uint8_t)(sum >> 24);
    p[2 + n + 1] = (uint8_t)(sum >> 16);
    p[2 + n + 2] = (uint8_t)(sum >> 8);
    p[2 + n + 3] = (uint8_t)sum;
    return n + 6;
}

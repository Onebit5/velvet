// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/inflate.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * inflate: undoing deflate.
 */

#include "lib/deflate.h"
#include "lib/hash.h"

#include <stdbool.h>

/*
 * least significant bit first, which is worth stating because it is the
 * opposite of how the huffman codes themselves are written down. deflate
 * packs bits into bytes from the bottom up, but a huffman code's bits go
 * out most significant first, so decoding a code means taking bits one
 * at a time from the bottom of the stream and appending each to the
 * *bottom* of the code being built.
 *
 * getting that backwards produces a decoder that reads a plausible
 * number of bits and gets nonsense, which is the single easiest thing to
 * get wrong in the whole format.
 */

struct bits {
    const uint8_t *data;
    size_t         len;
    size_t         at;      /* the next byte to take */
    uint32_t       hold;    /* bits taken and not yet used */
    int            count;   /* how many of them there are */
};

static int bits_get(struct bits *b, int want)
{
    while (b->count < want) {
        if (b->at >= b->len) {
            return -1;
        }
        b->hold |= (uint32_t)b->data[b->at++] << b->count;
        b->count += 8;
    }
    int value = (int)(b->hold & ((1u << want) - 1));
    b->hold >>= want;
    b->count -= want;
    return value;
}

/* a canonical code is defined entirely by the *lengths*. */

#define MAX_BITS 15

struct huff {
    short count[MAX_BITS + 1];
    short symbol[288];
};

static void huff_build(struct huff *h, const uint8_t *lengths, int n)
{
    for (int i = 0; i <= MAX_BITS; i++) {
        h->count[i] = 0;
    }
    for (int i = 0; i < n; i++) {
        h->count[lengths[i]]++;
    }
    h->count[0] = 0;        /* a length of zero means "not used at all" */

    short offset[MAX_BITS + 2];
    offset[1] = 0;
    for (int len = 1; len <= MAX_BITS; len++) {
        offset[len + 1] = (short)(offset[len] + h->count[len]);
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i] != 0) {
            h->symbol[offset[lengths[i]]++] = (short)i;
        }
    }
}

static int huff_decode(struct bits *b, const struct huff *h)
{
    int code = 0, first = 0, index = 0;

    for (int len = 1; len <= MAX_BITS; len++) {
        int bit = bits_get(b, 1);
        if (bit < 0) {
            return -1;
        }
        code |= bit;
        int count = h->count[len];
        /* the codes of this length occupy `count` consecutive values starting at `first`. */
        if (code - first < count) {
            return h->symbol[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;              /* longer than any code: the stream is wrong */
}

/*
 * a match is a symbol plus some extra bits read straight from the
 * stream, because 258 lengths and 32768 distances would need codes far
 * longer than the ranges deserve. the tables are the ones in the RFC and
 * there is nothing to derive: they are a choice somebody made in 1996.
 */

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

struct sink {
    uint8_t *out;
    size_t   cap;
    size_t   len;
};

static bool emit(struct sink *s, uint8_t byte)
{
    if (s->len >= s->cap) {
        return false;
    }
    s->out[s->len++] = byte;
    return true;
}

static long codes(struct bits *b, struct sink *s, const struct huff *lit,
                  const struct huff *dist)
{
    for (;;) {
        int sym = huff_decode(b, lit);
        if (sym < 0) {
            return INFLATE_BAD_INPUT;
        }
        if (sym < 256) {
            if (!emit(s, (uint8_t)sym)) {
                return INFLATE_NO_ROOM;
            }
            continue;
        }
        if (sym == 256) {
            return 0;               /* end of block */
        }

        sym -= 257;
        if (sym >= 29) {
            return INFLATE_BAD_INPUT;
        }
        int extra = bits_get(b, length_extra[sym]);
        if (extra < 0) {
            return INFLATE_BAD_INPUT;
        }
        unsigned len = length_base[sym] + (unsigned)extra;

        int dsym = huff_decode(b, dist);
        if (dsym < 0 || dsym >= 30) {
            return INFLATE_BAD_INPUT;
        }
        extra = bits_get(b, dist_extra[dsym]);
        if (extra < 0) {
            return INFLATE_BAD_INPUT;
        }
        unsigned back = dist_base[dsym] + (unsigned)extra;

        if (back > s->len) {
            return INFLATE_BAD_INPUT;   /* before the start of the output */
        }

        /* copied one byte at a time on purpose, and *not* with memcpy. */
        for (unsigned i = 0; i < len; i++) {
            if (!emit(s, s->out[s->len - back])) {
                return INFLATE_NO_ROOM;
            }
        }
    }
}

/* the table both ends already know. */
static void fixed_tables(struct huff *lit, struct huff *dist)
{
    uint8_t lengths[288];
    int i = 0;
    while (i < 144) { lengths[i++] = 8; }
    while (i < 256) { lengths[i++] = 9; }
    while (i < 280) { lengths[i++] = 7; }
    while (i < 288) { lengths[i++] = 8; }
    huff_build(lit, lengths, 288);

    for (i = 0; i < 30; i++) {
        lengths[i] = 5;
    }
    huff_build(dist, lengths, 30);
}

/* the table this block brought with it. */
static long dynamic_tables(struct bits *b, struct huff *lit, struct huff *dist)
{
    static const unsigned char order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    int nlen = bits_get(b, 5);
    int ndist = bits_get(b, 5);
    int ncode = bits_get(b, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) {
        return INFLATE_BAD_INPUT;
    }
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > 286 || ndist > 30) {
        return INFLATE_BAD_INPUT;
    }

    uint8_t lengths[320];
    for (int i = 0; i < 19; i++) {
        lengths[i] = 0;
    }
    for (int i = 0; i < ncode; i++) {
        int v = bits_get(b, 3);
        if (v < 0) {
            return INFLATE_BAD_INPUT;
        }
        lengths[order[i]] = (uint8_t)v;
    }

    struct huff code_lengths;
    huff_build(&code_lengths, lengths, 19);

    int have = 0;
    while (have < nlen + ndist) {
        int sym = huff_decode(b, &code_lengths);
        if (sym < 0) {
            return INFLATE_BAD_INPUT;
        }
        if (sym < 16) {
            lengths[have++] = (uint8_t)sym;
            continue;
        }

        /* 16 repeats the last length, 17 and 18 are runs of zero. */
        uint8_t repeat = 0;
        int times;
        if (sym == 16) {
            if (have == 0) {
                return INFLATE_BAD_INPUT;
            }
            repeat = lengths[have - 1];
            times = bits_get(b, 2);
            if (times < 0) {
                return INFLATE_BAD_INPUT;
            }
            times += 3;
        } else if (sym == 17) {
            times = bits_get(b, 3);
            if (times < 0) {
                return INFLATE_BAD_INPUT;
            }
            times += 3;
        } else {
            times = bits_get(b, 7);
            if (times < 0) {
                return INFLATE_BAD_INPUT;
            }
            times += 11;
        }
        if (have + times > nlen + ndist) {
            return INFLATE_BAD_INPUT;
        }
        while (times-- > 0) {
            lengths[have++] = repeat;
        }
    }

    if (lengths[256] == 0) {
        return INFLATE_BAD_INPUT;   /* no way to say the block has ended */
    }

    huff_build(lit, lengths, nlen);
    huff_build(dist, lengths + nlen, ndist);
    return 0;
}

long inflate(const void *in, size_t in_len, void *out, size_t out_cap)
{
    struct bits b = { in, in_len, 0, 0, 0 };
    struct sink s = { out, out_cap, 0 };

    int last = 0;
    do {
        last = bits_get(&b, 1);
        int type = bits_get(&b, 2);
        if (last < 0 || type < 0) {
            return INFLATE_BAD_INPUT;
        }

        if (type == 0) {
            /*
             * stored: the rest of the byte is thrown away, then a length
             * and its complement, which is the only integrity check
             * anywhere inside deflate itself
             */
            b.hold = 0;
            b.count = 0;
            if (b.at + 4 > b.len) {
                return INFLATE_BAD_INPUT;
            }
            unsigned len = (unsigned)b.data[b.at] | ((unsigned)b.data[b.at + 1] << 8);
            unsigned nlen = (unsigned)b.data[b.at + 2] | ((unsigned)b.data[b.at + 3] << 8);
            b.at += 4;
            if ((len ^ 0xffffu) != nlen) {
                return INFLATE_BAD_INPUT;
            }
            if (b.at + len > b.len) {
                return INFLATE_BAD_INPUT;
            }
            for (unsigned i = 0; i < len; i++) {
                if (!emit(&s, b.data[b.at + i])) {
                    return INFLATE_NO_ROOM;
                }
            }
            b.at += len;
        } else if (type == 1 || type == 2) {
            struct huff lit, dist;
            if (type == 1) {
                fixed_tables(&lit, &dist);
            } else {
                long bad = dynamic_tables(&b, &lit, &dist);
                if (bad != 0) {
                    return bad;
                }
            }
            long bad = codes(&b, &s, &lit, &dist);
            if (bad != 0) {
                return bad;
            }
        } else {
            return INFLATE_BAD_INPUT;   /* type 3 has never meant anything */
        }
    } while (last == 0);

    return (long)s.len;
}

long zlib_inflate(const void *in, size_t in_len, void *out, size_t out_cap)
{
    const uint8_t *p = in;
    if (in_len < 6) {
        return INFLATE_BAD_INPUT;
    }

    /*
     * two bytes of header. the low nibble of the first says the method
     *, 8 is deflate and nothing else was ever defined, the high
     * nibble says how big a window was used, and the pair has to be a
     * multiple of 31, which is a checksum small enough to be worth
     * nothing and cheap enough to be worth having
     */
    unsigned cmf = p[0], flg = p[1];
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7) {
        return INFLATE_BAD_INPUT;
    }
    if ((cmf * 256 + flg) % 31 != 0) {
        return INFLATE_BAD_INPUT;
    }
    if (flg & 0x20) {
        return INFLATE_BAD_INPUT;   /* a preset dictionary, which git never uses */
    }

    long n = inflate(p + 2, in_len - 2 - 4, out, out_cap);
    if (n < 0) {
        return n;
    }

    /* and the adler32 of what came out, big-endian at the end. */
    const uint8_t *tail = p + in_len - 4;
    uint32_t want = ((uint32_t)tail[0] << 24) | ((uint32_t)tail[1] << 16)
                  | ((uint32_t)tail[2] << 8) | (uint32_t)tail[3];
    if (adler32_of(out, (size_t)n) != want) {
        return INFLATE_BAD_CHECK;
    }
    return n;
}

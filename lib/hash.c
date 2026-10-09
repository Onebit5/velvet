// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/hash.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the three sums this machine has any use for.
 */

#include <velvet/hash.h>

/* a byte copy of its own rather than memcpy from <velvet/string.h>. */
static void copy_bytes(uint8_t *to, const uint8_t *from, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        to[i] = from[i];
    }
}

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

/* two running sums: one of the bytes, one of the sums. */
uint32_t adler32_more(uint32_t so_far, const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t a = so_far & 0xffff;
    uint32_t b = (so_far >> 16) & 0xffff;

    while (len > 0) {
        size_t chunk = len < 5552 ? len : 5552;
        len -= chunk;
        while (chunk-- > 0) {
            a += *p++;
            b += a;
        }
        a %= 65521;
        b %= 65521;
    }
    return (b << 16) | a;
}

uint32_t adler32_of(const void *data, size_t len)
{
    return adler32_more(adler32_start(), data, len);
}

/*
 * eighty rounds over a sixteen-word block, with the block first stretched
 * into eighty words by xoring four earlier ones together and rotating.
 * the five constants and the four round functions are the whole of it.
 *
 * everything about the shape of this is big-endian, the words are read
 * big-endian, the length is appended big-endian, which is worth saying
 * on a machine that is not, because every one of those byte swaps is a
 * chance to produce a digest that is beautifully self-consistent and
 * agrees with nobody.
 */

static uint32_t rol(uint32_t v, int by)
{
    return (v << by) | (v >> (32 - by));
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

static void sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80];

    for (int i = 0; i < 16; i++) {
        w[i] = be32(p + i * 4);
    }
    for (int i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }

    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void sha1_init(struct sha1 *s)
{
    s->h[0] = 0x67452301;
    s->h[1] = 0xefcdab89;
    s->h[2] = 0x98badcfe;
    s->h[3] = 0x10325476;
    s->h[4] = 0xc3d2e1f0;
    s->bytes = 0;
    s->have = 0;
}

/* the part that is easy to get wrong, and it is not the arithmetic. */
void sha1_update(struct sha1 *s, const void *data, size_t len)
{
    const uint8_t *p = data;
    s->bytes += len;

    if (s->have > 0) {
        size_t want = 64 - s->have;
        size_t take = len < want ? len : want;
        copy_bytes(s->block + s->have, p, take);
        s->have += take;
        p += take;
        len -= take;
        if (s->have < 64) {
            return;         /* still not a whole block */
        }
        sha1_block(s->h, s->block);
        s->have = 0;
    }

    while (len >= 64) {
        sha1_block(s->h, p);
        p += 64;
        len -= 64;
    }

    copy_bytes(s->block, p, len);
    s->have = len;
}

void sha1_final(struct sha1 *s, uint8_t out[20])
{
    /* a one bit, then zeros, then the length in bits as eight bytes. */
    uint64_t bits = s->bytes * 8;
    uint8_t tail[72];
    size_t pad = 0;

    tail[pad++] = 0x80;
    while ((s->bytes + pad) % 64 != 56) {
        tail[pad++] = 0;
    }
    for (int i = 7; i >= 0; i--) {
        tail[pad++] = (uint8_t)(bits >> (i * 8));
    }

    /*
     * fed through update, so the block buffering is the same code that
     * everything else went through rather than a second copy of it
     */
    uint64_t was = s->bytes;
    sha1_update(s, tail, pad);
    s->bytes = was;

    for (int i = 0; i < 5; i++) {
        out[i * 4 + 0] = (uint8_t)(s->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)s->h[i];
    }
}

void sha1_of(const void *data, size_t len, uint8_t out[20])
{
    struct sha1 s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, out);
}

void sha1_hex(const uint8_t digest[20], char out[41])
{
    static const char digit[] = "0123456789abcdef";
    for (int i = 0; i < 20; i++) {
        out[i * 2 + 0] = digit[digest[i] >> 4];
        out[i * 2 + 1] = digit[digest[i] & 0x0f];
    }
    out[40] = '\0';
}

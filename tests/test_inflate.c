// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_inflate.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * deflate and inflate, against zlib and against themselves.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "lib/deflate.h"

static int failures;

#define CHECK(cond, what) do {                                          \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);     \
            failures++;                                                 \
        }                                                               \
    } while (0)

static uint8_t packed[1 << 21];
static uint8_t back[1 << 20];

static bool round_trip(const void *data, size_t len, const char *what)
{
    long n = zlib_deflate(data, len, packed, sizeof packed);
    if (n < 0) {
        printf("  FAIL %s: would not compress (%ld)\n", what, n);
        failures++;
        return false;
    }
    long m = zlib_inflate(packed, (size_t)n, back, sizeof back);
    if (m != (long)len || memcmp(back, data, len) != 0) {
        printf("  FAIL %s: %zu bytes became %ld and came back as %ld\n",
               what, len, n, m);
        failures++;
        return false;
    }
    return true;
}

static void test_round_trips(void)
{
    round_trip("", 0, "nothing at all");
    round_trip("x", 1, "one byte");
    round_trip("hello hello hello hello", 23, "something repetitive");

    /*
     * a run of one byte, which is the case lz77 writes as "go back one
     * and copy two hundred", so the source and the destination of the
     * copy overlap. a decompressor using memcpy for that is wrong in a
     * way that only shows up here
     */
    static uint8_t run[5000];
    memset(run, 'a', sizeof run);
    round_trip(run, sizeof run, "five thousand of the same byte");

    /*
     * incompressible: every block would come out bigger, which is what
     * the stored fallback exists for
     */
    static uint8_t noise[20000];
    uint32_t seed = 12345;
    for (size_t i = 0; i < sizeof noise; i++) {
        seed = seed * 1103515245u + 12345u;
        noise[i] = (uint8_t)(seed >> 16);
    }
    round_trip(noise, sizeof noise, "twenty thousand random bytes");

    /*
     * something with real structure, and long enough that matches reach
     * back further than a short window would allow
     */
    static uint8_t text[200000];
    const char *line = "the quick brown fox jumps over the lazy dog\n";
    size_t at = 0;
    while (at + 44 < sizeof text) {
        memcpy(text + at, line, 43);
        at += 43;
        text[at - 1] = (uint8_t)('a' + (at % 26));
    }
    if (round_trip(text, at, "two hundred thousand bytes of text")) {
        /* and it has to actually compress. */
        long n = zlib_deflate(text, at, packed, sizeof packed);
        CHECK(n > 0 && (size_t)n < at / 10,
              "repetitive text compresses to under a tenth of itself");
    }
}

static void test_refusals(void)
{
    /*
     * a decompressor that accepts nonsense is the one thing here that
     * is actually dangerous: the input is a file somebody else wrote,
     * and the output goes in a buffer of ours
     */
    long n = zlib_deflate("hello hello hello", 17, packed, sizeof packed);
    CHECK(n > 0, "it compressed at all");

    uint8_t copy[64];
    memcpy(copy, packed, (size_t)n);

    copy[n - 1] ^= 0xff;
    CHECK(zlib_inflate(copy, (size_t)n, back, sizeof back) == INFLATE_BAD_CHECK,
          "a wrong adler32 is refused, not returned");

    memcpy(copy, packed, (size_t)n);
    copy[0] = 0x77;         /* no longer a multiple of 31 */
    CHECK(zlib_inflate(copy, (size_t)n, back, sizeof back) == INFLATE_BAD_INPUT,
          "a header that is not a zlib header is refused");

    memcpy(copy, packed, (size_t)n);
    CHECK(zlib_inflate(copy, 4, back, sizeof back) < 0,
          "and one that stops in the middle");

    /*
     * the important one: it must refuse to write past what it was
     * given, however much the stream claims to hold
     */
    static uint8_t run[9000];
    memset(run, 'z', sizeof run);
    long big = zlib_deflate(run, sizeof run, packed, sizeof packed);
    CHECK(big > 0, "the run compressed");
    CHECK(zlib_inflate(packed, (size_t)big, back, 100) == INFLATE_NO_ROOM,
          "nine thousand bytes will not be unpacked into a hundred");
}

static void test_stored(void)
{
    /*
     * a stored block by hand: the final-block bit, type 0, and then the
     * length and its complement. this is what `zlib.compress(data, 0)`
     * produces and what the roadmap called the cheat, worth being
     * able to read whether or not this ever writes one
     */
    static const uint8_t stream[] = {
        0x78, 0x01,                     /* zlib header */
        0x01, 0x05, 0x00, 0xfa, 0xff,   /* last, stored, len 5, ~len */
        'h', 'e', 'l', 'l', 'o',
        0x06, 0x2c, 0x02, 0x15,         /* adler32 of "hello" */
    };
    long n = zlib_inflate(stream, sizeof stream, back, sizeof back);
    CHECK(n == 5 && memcmp(back, "hello", 5) == 0,
          "a stored block reads back as itself");
}



static uint8_t input[1 << 22];

int main(int argc, char **argv)
{
    if (argc >= 2) {
        size_t len = fread(input, 1, sizeof input, stdin);
        long n;
        if (strcmp(argv[1], "--deflate") == 0) {
            n = zlib_deflate(input, len, packed, sizeof packed);
        } else if (strcmp(argv[1], "--inflate") == 0)
{
            n = zlib_inflate(input, len, packed, sizeof packed);
        } else if (strcmp(argv[1], "--raw-inflate") == 0)
{
            n = inflate(input, len, packed, sizeof packed);
        } else {
            fprintf(stderr, "inflate: unknown %s\n", argv[1]);
            return 2;
        }
        if (n < 0) {
            fprintf(stderr, "inflate: %ld\n", n);
            return 1;
        }
        fwrite(packed, 1, (size_t)n, stdout);
        return 0;
    }

    test_round_trips();
    test_refusals();
    test_stored();

    if (failures > 0) {
        printf("  %d failed\n", failures);
        return 1;
    }
    return 0;
}

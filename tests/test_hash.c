// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_hash.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the three sums, against the numbers everybody else got.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "lib/hash.h"

static int failures;

#define CHECK(cond, what) do {                                          \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);     \
            failures++;                                                 \
        }                                                               \
    } while (0)

static const char *hex_of(const void *data, size_t len)
{
    static char out[41];
    uint8_t digest[20];
    sha1_of(data, len, digest);
    sha1_hex(digest, out);
    return out;
}

static void test_sha1_vectors(void)
{
    /*
     * the ones in the standard, plus the empty string, which is the
     * case an implementation gets wrong by not padding at all
     */
    CHECK(strcmp(hex_of("", 0),
                 "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0,
          "sha1 of nothing is the one everybody has seen");
    CHECK(strcmp(hex_of("abc", 3),
                 "a9993e364706816aba3e25717850c26c9cd0d89d") == 0,
          "sha1 of abc");
    CHECK(strcmp(hex_of("abcdbcdecdefdefgefghfghighijhi"
                        "jkijkljklmklmnlmnomnopnopq", 56),
                 "84983e441c3bd26ebaae4aa1f95129e5e54670f1") == 0,
          "sha1 of the two-block vector");

    /*
     * a million a's: the vector that exists because it is more than any
     * buffer, and the only one that ever catches a length counted in
     * bytes where it should be bits
     */
    char *big = malloc(1000000);
    memset(big, 'a', 1000000);
    CHECK(strcmp(hex_of(big, 1000000),
                 "34aa973cd4c4daa4f61eeb2bdbad27316534016f") == 0,
          "sha1 of a million a's");
    free(big);
}

static void test_sha1_lengths(void)
{
    /* every length across a block boundary. */
    uint8_t data[200];
    for (size_t i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 7 + 1);
    }

    for (size_t len = 0; len <= 130; len++) {
        uint8_t whole[20], piecemeal[20];
        sha1_of(data, len, whole);

        /* the same bytes one at a time. */
        struct sha1 s;
        sha1_init(&s);
        for (size_t i = 0; i < len; i++) {
            sha1_update(&s, data + i, 1);
        }
        sha1_final(&s, piecemeal);
        CHECK(memcmp(whole, piecemeal, 20) == 0,
              "a byte at a time is the same as all at once");

        /*
         * and in sevens, which is neither a factor nor a multiple of
         * 64 and so leaves a different amount over every time
         */
        sha1_init(&s);
        for (size_t i = 0; i < len; i += 7) {
            size_t take = len - i < 7 ? len - i : 7;
            sha1_update(&s, data + i, take);
        }
        sha1_final(&s, piecemeal);
        CHECK(memcmp(whole, piecemeal, 20) == 0,
              "and in sevens");
    }
}

static void test_crc32(void)
{
    CHECK(crc32_of("123456789", 9) == 0xcbf43926u,
          "crc32 of 123456789 is the check value in the standard");
    CHECK(crc32_of("", 0) == 0, "crc32 of nothing is nothing");
    CHECK(crc32_of("a", 1) == 0xe8b7be43u, "crc32 of a");

    /* carrying on from a finished answer. */
    const char *all = "the quick brown fox";
    uint32_t once = crc32_of(all, strlen(all));
    uint32_t split = crc32_more(crc32_start(), all, 4);
    split = crc32_more(split, all + 4, strlen(all) - 4);
    CHECK(once == split, "crc32 in two goes is crc32 in one");
}

static void test_adler32(void)
{
    CHECK(adler32_of("", 0) == 1u,
          "adler32 of nothing is 1, not 0, the low half starts at one");
    CHECK(adler32_of("abc", 3) == 0x024d0127u, "adler32 of abc");
    CHECK(adler32_of("Wikipedia", 9) == 0x11e60398u,
          "adler32 of the example everybody uses");

    const char *all = "the quick brown fox";
    uint32_t once = adler32_of(all, strlen(all));
    uint32_t split = adler32_more(adler32_start(), all, 4);
    split = adler32_more(split, all + 4, strlen(all) - 4);
    CHECK(once == split, "adler32 in two goes is adler32 in one");

    /*
     * longer than the 5552 bytes it may go without taking a modulus,
     * which is the only interesting thing about the arithmetic
     */
    uint8_t *big = malloc(20000);
    memset(big, 0xff, 20000);
    uint32_t whole = adler32_of(big, 20000);
    uint32_t parts = adler32_start();
    for (int i = 0; i < 20; i++) {
        parts = adler32_more(parts, big + i * 1000, 1000);
    }
    CHECK(whole == parts, "and past the point where it has to reduce");

    /* the same input against an answer from *outside* this file, which the line above cannot be. */
    CHECK(whole == 0x9f51d664u,
          "adler32 of twenty thousand 0xff, as zlib has it");
    free(big);

    uint8_t *mixed = malloc(70000);
    for (int i = 0; i < 70000; i++) {
        mixed[i] = (uint8_t)(i * 7 + 3);
    }
    CHECK(adler32_of(mixed, 70000) == 0x8e7b36c1u,
          "and of seventy thousand mixed bytes");
    free(mixed);
}



static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = strcmp(path, "-") == 0 ? stdin : fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "hash: cannot read %s\n", path);
        exit(2);
    }
    size_t cap = 65536, have = 0;
    uint8_t *buf = malloc(cap);
    size_t n;
    while ((n = fread(buf + have, 1, cap - have, f)) > 0) {
        have += n;
        if (have == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    if (f != stdin) {
        fclose(f);
    }
    *len = have;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc >= 3) {
        size_t len;
        uint8_t *data = slurp(argv[2], &len);
        char hex[41];
        uint8_t digest[20];

        /*
         * how git names a blob: the type, a space, the length in
         * decimal, a zero byte, and then the content. the header is
         * hashed *with* the data, which is why a git object's name
         * cannot be worked out from the file alone, and why an empty
         * file still has a name
         */
        if (strcmp(argv[1], "--git-blob") == 0) {
            char header[64];
            int n = snprintf(header, sizeof header, "blob %zu", len);
            header[n] = '\0';
            struct sha1 s;
            sha1_init(&s);
            sha1_update(&s, header, (size_t)n + 1);
            sha1_update(&s, data, len);
            sha1_final(&s, digest);
            sha1_hex(digest, hex);
            printf("%s\n", hex);
            return 0;
        }
        if (strcmp(argv[1], "--sha1") == 0) {
            sha1_of(data, len, digest);
            sha1_hex(digest, hex);
            printf("%s\n", hex);
            return 0;
        }
        if (strcmp(argv[1], "--crc32") == 0) {
            printf("%08x\n", crc32_of(data, len));
            return 0;
        }
        if (strcmp(argv[1], "--adler32") == 0) {
            printf("%08x\n", adler32_of(data, len));
            return 0;
        }
        /*
         * the same file fed in pieces of whatever size was asked for,
         * so the check can prove that how the data arrives makes no
         * difference to the answer
         */
        if (strncmp(argv[1], "--sha1-chunked=", 15) == 0) {
            size_t chunk = (size_t)atoi(argv[1] + 15);
            struct sha1 s;
            sha1_init(&s);
            for (size_t i = 0; i < len; i += chunk) {
                size_t take = len - i < chunk ? len - i : chunk;
                sha1_update(&s, data + i, take);
            }
            sha1_final(&s, digest);
            sha1_hex(digest, hex);
            printf("%s\n", hex);
            return 0;
        }
        fprintf(stderr, "hash: unknown %s\n", argv[1]);
        return 2;
    }

    test_sha1_vectors();
    test_sha1_lengths();
    test_crc32();
    test_adler32();

    if (failures > 0) {
        printf("  %d failed\n", failures);
        return 1;
    }
    return 0;
}

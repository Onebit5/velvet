// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_hash.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * crc32, against the numbers everybody else got.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <velvet/hash.h>

static int failures;

#define CHECK(cond, what) do {                                          \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);     \
            failures++;                                                 \
        }                                                               \
    } while (0)

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

        if (strcmp(argv[1], "--crc32") == 0) {
            printf("%08x\n", crc32_of(data, len));
            return 0;
        }
        /*
         * the same file fed in pieces of whatever size was asked for,
         * so the check can prove that how the data arrives makes no
         * difference to the answer
         */
        if (strncmp(argv[1], "--crc32-chunked=", 16) == 0) {
            size_t chunk = (size_t)atoi(argv[1] + 16);
            uint32_t crc = crc32_start();
            for (size_t i = 0; i < len; i += chunk) {
                size_t take = len - i < chunk ? len - i : chunk;
                crc = crc32_more(crc, data + i, take);
            }
            printf("%08x\n", crc);
            return 0;
        }
        fprintf(stderr, "hash: unknown %s\n", argv[1]);
        return 2;
    }

    test_crc32();

    if (failures > 0) {
        printf("  %d failed\n", failures);
        return 1;
    }
    return 0;
}

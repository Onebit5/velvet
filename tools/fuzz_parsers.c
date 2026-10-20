// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/fuzz_parsers.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 *
 * a fuzzer for the parsers that take hostile input. feeds random bytes and
 * mutations of a valid seed to tcp and ustar, looking for a
 * crash, a hang, or a trap from ubsan in trap mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <velvet/tcp.h>
#include "fs/ustar.h"

#define ROUNDS   200000
#define BUF      1024

#define TCP_FROM 0x0a000202
#define TCP_TO   0x0a00020f

static uint64_t rng_state = 0x2545f4914f6cdd1dull;

static uint32_t rnd(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 32);
}

static uint8_t buf[BUF];
static uint8_t out[BUF];
static uint8_t pay[64];

static size_t fill(size_t most)
{
    size_t n = rnd() % (most + 1);

    for (size_t i = 0; i < n; i++) {
        /* mostly small values, so headers and lengths look plausible */
        buf[i] = (rnd() % 4 == 0) ? (uint8_t)rnd() : (uint8_t)(rnd() % 32);
    }
    return n;
}

static void mutate(size_t n)
{
    if (n && rnd() % 2) {
        buf[rnd() % n] = (uint8_t)rnd();
    }
}

/*
 * a valid tcp segment, built rather than written out so the checksum is
 * right. tcp_parse checks the checksum before reading a single field, so a
 * seed written by hand with zeros in those two bytes tests the checksum and
 * nothing else, which is what the first version of this file did.
 */
static size_t seed_tcp(void)
{
    return tcp_build(buf, TCP_FROM, TCP_TO, 4444, 80, 1, 1, 0x18, 0x1000,
                     "hello", 5);
}

static void fuzz_tcp(void)
{
    for (int i = 0; i < ROUNDS; i++) {
        struct tcp_segment seg;
        size_t n;

        if (i % 2 == 0) {
            size_t plen = rnd() % sizeof pay;

            for (size_t j = 0; j < plen; j++) {
                pay[j] = (uint8_t)rnd();
            }
            /* built, so the checksum is right and the fields get read */
            n = tcp_build(buf, TCP_FROM, TCP_TO, (uint16_t)rnd(),
                          (uint16_t)rnd(), rnd(), rnd(), (uint8_t)rnd(),
                          (uint16_t)rnd(), pay, plen);
        } else {
            n = fill(60);       /* and garbage, which it has to refuse */
        }
        tcp_parse(buf, n, TCP_FROM, TCP_TO, &seg);
    }
}

static void fuzz_ustar(void)
{
    for (int i = 0; i < ROUNDS; i++) {
        struct tar_header h;

        memset(&h, 0, sizeof h);
        memcpy(&h, buf, fill(sizeof h));

        /* an octal field of the wrong length, and a size that never ends */
        ustar_octal(h.size, sizeof h.size);
        ustar_octal(h.name, rnd() % 16);
        ustar_next(rnd(), &h);
    }
}

/*
 * a seed that is refused means the fuzzer is testing nothing, so it says so
 * rather than printing a clean result. this check is cheap, and its absence
 * is why the first version reported a tcp result worth nothing.
 */
static void require_seeds_parse(void)
{
    struct tcp_segment seg;
    size_t n;

    n = seed_tcp();
    if (!tcp_parse(buf, n, TCP_FROM, TCP_TO, &seg)) {
        printf("the tcp seed is refused: the parser would never be reached\n");
        exit(1);
    }

    printf("seeds parse: tcp reaches the parser it is meant to\n");
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        rng_state = strtoull(argv[1], NULL, 0);
    }

    printf("fuzzing tcp, ustar: %d rounds each, seed %llx\n",
           ROUNDS, (unsigned long long)rng_state);
    require_seeds_parse();
    fflush(stdout);
    fuzz_tcp();
    printf("  tcp   ok\n");
    fflush(stdout);
    fuzz_ustar();
    printf("  ustar ok\n");
    printf("no crashes, no hangs, no traps\n");
    return 0;
}

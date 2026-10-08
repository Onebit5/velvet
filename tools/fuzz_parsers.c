// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/fuzz_parsers.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 *
 * a fuzzer for the parsers that take hostile input. feeds random bytes and
 * mutations of a valid seed to dns, http, tcp and ustar, looking for a
 * crash, a hang, or a trap from ubsan in trap mode.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "net/dns.h"
#include "net/http.h"
#include "net/tcp.h"
#include "fs/ustar.h"

#define ROUNDS   200000
#define BUF      1024

#define DNS_ID   0x1234
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

/* a valid dns response, so the mutations start somewhere real */
static size_t seed_dns(void)
{
    const uint8_t seed[] = {
        0x12, 0x34, 0x81, 0x80, 0x00, 0x01, 0x00, 0x01,
        0x00, 0x00, 0x00, 0x00,
        0x03, 'w', 'w', 'w', 0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
        0x03, 'c', 'o', 'm', 0x00,
        0x00, 0x01, 0x00, 0x01,
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c,
        0x00, 0x04, 10, 0, 2, 2,
    };
    memcpy(buf, seed, sizeof seed);
    return sizeof seed;
}

/* a valid http response, fed in pieces to exercise the state machine */
static size_t seed_http(void)
{
    const char *s = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                    "Content-Type: text/plain\r\n\r\nhello";
    size_t n = strlen(s);

    memcpy(buf, s, n);
    return n;
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

static void fuzz_dns(void)
{
    static const char *names[] = { "www.example.com", "a", "", "x.y.z",
                                   "aaaaaaaaaaaaaaaaaaaaaaaa" };
    for (int i = 0; i < ROUNDS; i++) {
        size_t n;
        ipv4 addr;
        uint32_t ttl;

        if (i % 4 == 0) {
            n = seed_dns();
            mutate(n);
        } else {
            n = fill(sizeof buf);
            /*
             * the parser checks the transaction id and the answer bit before
             * it reads anything else. satisfy both, or the random input only
             * ever tests those two checks.
             */
            if (n >= 12) {
                buf[0] = 0x12;
                buf[1] = 0x34;
                buf[2] |= 0x80;
            }
        }
        dns_parse_response(buf, n, DNS_ID,
                           names[rnd() % (sizeof names / sizeof *names)],
                           &addr, &ttl);
    }
}

static void fuzz_http(void)
{
    for (int i = 0; i < ROUNDS; i++) {
        struct http h;
        size_t n, out_len = 0, at = 0;

        memset(&h, 0, sizeof h);
        n = (i % 4 == 0) ? seed_http() : fill(sizeof buf);
        if (i % 4 == 0) {
            mutate(n);
        }

        /* in pieces, since a response arrives in whatever tcp delivers */
        while (at < n) {
            size_t piece = 1 + rnd() % 8;

            if (piece > n - at) {
                piece = n - at;
            }
            http_feed(&h, buf + at, piece, out, sizeof out, &out_len);
            at += piece;
        }
    }
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
    ipv4 addr;
    uint32_t ttl;
    size_t n;

    n = seed_dns();
    if (dns_parse_response(buf, n, DNS_ID, "www.example.com", &addr,
                           &ttl) != DNS_OK) {
        printf("the dns seed is refused: the parser would never be reached\n");
        exit(1);
    }

    n = seed_tcp();
    if (!tcp_parse(buf, n, TCP_FROM, TCP_TO, &seg)) {
        printf("the tcp seed is refused: the parser would never be reached\n");
        exit(1);
    }

    printf("seeds parse: dns and tcp reach the parsers they are meant to\n");
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        rng_state = strtoull(argv[1], NULL, 0);
    }

    printf("fuzzing dns, http, tcp, ustar: %d rounds each, seed %llx\n",
           ROUNDS, (unsigned long long)rng_state);
    require_seeds_parse();
    fflush(stdout);
    fuzz_dns();
    printf("  dns   ok\n");
    fflush(stdout);
    fuzz_http();
    printf("  http  ok\n");
    fflush(stdout);
    fuzz_tcp();
    printf("  tcp   ok\n");
    fflush(stdout);
    fuzz_ustar();
    printf("  ustar ok\n");
    printf("no crashes, no hangs, no traps\n");
    return 0;
}

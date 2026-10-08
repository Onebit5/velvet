// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_dns.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for dns.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "net/net.h"
#include "net/dns.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* read a name out of a buffer that ends exactly where the message does. */
static bool read_name_exact(const uint8_t *p, size_t len, size_t at,
                            char *out, size_t max, size_t *after)
{
    uint8_t *exact = malloc(len);
    memcpy(exact, p, len);
    bool ok = dns_read_name(exact, len, at, out, max, after);
    free(exact);
    return ok;
}

static enum dns_result parse_exact(const uint8_t *p, size_t len, uint16_t id,
                                   const char *name, ipv4 *out,
                                   uint32_t *ttl)
{
    uint8_t *exact = malloc(len);
    memcpy(exact, p, len);
    enum dns_result r = dns_parse_response(exact, len, id, name, out, ttl);
    free(exact);
    return r;
}



static size_t put_header(uint8_t *p, uint16_t id, uint16_t flags,
                         uint16_t qd, uint16_t an)
{
    p[0] = (uint8_t)(id >> 8);    p[1] = (uint8_t)id;
    p[2] = (uint8_t)(flags >> 8); p[3] = (uint8_t)flags;
    p[4] = (uint8_t)(qd >> 8);    p[5] = (uint8_t)qd;
    p[6] = (uint8_t)(an >> 8);    p[7] = (uint8_t)an;
    p[8] = 0; p[9] = 0; p[10] = 0; p[11] = 0;
    return 12;
}

/* "www.example.com" -> 3www7example3com0 */
static size_t put_name(uint8_t *p, size_t at, const char *name)
{
    while (*name) {
        const char *dot = name;
        while (*dot && *dot != '.') {
            dot++;
        }
        size_t n = (size_t)(dot - name);
        p[at++] = (uint8_t)n;
        memcpy(p + at, name, n);
        at += n;
        name = (*dot == '.') ? dot + 1 : dot;
    }
    p[at++] = 0;
    return at;
}

static size_t put_a(uint8_t *p, size_t at, const char *name, uint32_t ip,
                    uint32_t ttl)
{
    at = put_name(p, at, name);
    p[at++] = 0; p[at++] = 1;           /* type A */
    p[at++] = 0; p[at++] = 1;           /* class IN */
    p[at++] = (uint8_t)(ttl >> 24); p[at++] = (uint8_t)(ttl >> 16);
    p[at++] = (uint8_t)(ttl >> 8);  p[at++] = (uint8_t)ttl;
    p[at++] = 0; p[at++] = 4;           /* rdlength */
    p[at++] = (uint8_t)(ip >> 24); p[at++] = (uint8_t)(ip >> 16);
    p[at++] = (uint8_t)(ip >> 8);  p[at++] = (uint8_t)ip;
    return at;
}

/* a whole ordinary reply: header, the question echoed, one A record */
static size_t good_reply(uint8_t *p, uint16_t id, const char *name,
                         uint32_t ip)
{
    size_t at = put_header(p, id, 0x8180, 1, 1);
    at = put_name(p, at, name);
    p[at++] = 0; p[at++] = 1;
    p[at++] = 0; p[at++] = 1;
    return put_a(p, at, name, ip, 300);
}

int main(void)
{
    uint8_t p[600];
    char name[DNS_NAME_MAX + 1];
    size_t at, after, n;
    ipv4 got;
    uint32_t ttl;



    n = dns_build_query(p, sizeof p, "www.example.com", 0x1234);
    CHECK(n > 0, "a query is built");
    CHECK(p[0] == 0x12 && p[1] == 0x34, "carrying the id it was given");
    CHECK(p[2] == 0x01 && p[3] == 0x00,
          "a question, with recursion desired, this machine does not "
          "walk the tree itself");
    CHECK(p[4] == 0 && p[5] == 1, "one question");
    CHECK(p[6] == 0 && p[7] == 0, "and no answers in it");

    CHECK(p[12] == 3 && memcmp(p + 13, "www", 3) == 0,
          "the name goes on the wire as length-prefixed labels");
    CHECK(p[16] == 7 && memcmp(p + 17, "example", 7) == 0, "each of them");
    CHECK(p[24] == 3 && memcmp(p + 25, "com", 3) == 0, "to the last");
    CHECK(p[28] == 0, "ended by the root label");
    CHECK(p[29] == 0 && p[30] == 1, "asking for an A record");
    CHECK(p[31] == 0 && p[32] == 1, "in the internet class");
    CHECK(n == 33, "and nothing after it");

    /* names a query may not be built from */
    CHECK(dns_build_query(p, sizeof p, "", 1) == 0, "an empty name is not one");
    CHECK(dns_build_query(p, sizeof p, "a..b", 1) == 0,
          "and neither is one with an empty label");
    CHECK(dns_build_query(p, sizeof p, ".", 1) == 0, "or a bare dot");

    {
        char toolong[80];
        memset(toolong, 'a', sizeof toolong);
        toolong[sizeof toolong - 1] = '\0';
        CHECK(dns_build_query(p, sizeof p, toolong, 1) == 0,
              "a label over 63 bytes is refused rather than truncated");
    }
    {
        /* a name over 255 bytes, in legal labels */
        char big[400];
        size_t w = 0;
        for (int i = 0; i < 40; i++) {
            memcpy(big + w, "abcdefgh.", 9);
            w += 9;
        }
        big[w - 1] = '\0';
        CHECK(dns_build_query(p, sizeof p, big, 1) == 0,
              "and so is a whole name over 255");
    }

    /* a trailing dot is the root and is a real name */
    CHECK(dns_build_query(p, sizeof p, "example.com.", 1) > 0,
          "a trailing dot is the root, and is allowed");



    memset(p, 0, sizeof p);
    at = put_name(p, 0, "www.example.com");
    CHECK(dns_read_name(p, at, 0, name, sizeof name, &after),
          "an ordinary name decodes");
    CHECK(strcmp(name, "www.example.com") == 0, "with the dots put back");
    CHECK(after == at, "and says where it ended");



    /* a pointer to a name earlier in the message */
    memset(p, 0, sizeof p);
    at = put_name(p, 0, "example.com");
    size_t ptr_at = at;
    p[at++] = 0xc0; p[at++] = 0x00;     /* -> offset 0 */
    CHECK(dns_read_name(p, at, ptr_at, name, sizeof name, &after),
          "a pointer to an earlier name follows it");
    CHECK(strcmp(name, "example.com") == 0, "and decodes the same");
    CHECK(after == ptr_at + 2,
          "the name after a pointer continues past the *pointer*, not "
          "past what it pointed at, a pointer ends the name it is in");

    /* a label, then a pointer: "www" + -> "example.com" */
    memset(p, 0, sizeof p);
    at = put_name(p, 0, "example.com");
    size_t mixed = at;
    p[at++] = 3; p[at++] = 'w'; p[at++] = 'w'; p[at++] = 'w';
    p[at++] = 0xc0; p[at++] = 0x00;
    CHECK(dns_read_name(p, at, mixed, name, sizeof name, &after),
          "a name that is part labels and part pointer decodes");
    CHECK(strcmp(name, "www.example.com") == 0, "into the whole thing");

    /* a pointer at itself. this is the one that hangs a naive decoder */
    memset(p, 0, sizeof p);
    p[0] = 0xc0; p[1] = 0x00;
    CHECK(!read_name_exact(p, 2, 0, name, sizeof name, &after),
          "a pointer at itself is refused rather than followed forever");

    /* two pointers pointing at each other */
    memset(p, 0, sizeof p);
    p[0] = 0xc0; p[1] = 0x02;
    p[2] = 0xc0; p[3] = 0x00;
    CHECK(!read_name_exact(p, 4, 0, name, sizeof name, &after),
          "and so is a pair of them pointing at each other");

    /* a long chain that never ends, each pointing at the next */
    memset(p, 0, sizeof p);
    for (int i = 0; i < 100; i++) {
        p[i * 2] = 0xc0;
        p[i * 2 + 1] = (uint8_t)((i + 1) * 2);
    }
    CHECK(!read_name_exact(p, 200, 0, name, sizeof name, &after),
          "a chain of pointers is bounded rather than walked");

    /* a pointer off the end of the message */
    memset(p, 0, sizeof p);
    p[0] = 0xc0; p[1] = 0xff;
    CHECK(!read_name_exact(p, 2, 0, name, sizeof name, &after),
          "a pointer outside the message is refused, not read");

    /* half a pointer at the very end */
    memset(p, 0, sizeof p);
    p[0] = 0xc0;
    CHECK(!read_name_exact(p, 1, 0, name, sizeof name, &after),
          "and half of one is not a pointer at all");

    /* a label that runs past the end */
    memset(p, 0, sizeof p);
    p[0] = 40;
    CHECK(!read_name_exact(p, 10, 0, name, sizeof name, &after),
          "a label claiming more bytes than arrived is refused");

    /* a name that never ends */
    memset(p, 0, sizeof p);
    for (int i = 0; i < 100; i += 2) {
        p[i] = 1;
        p[i + 1] = 'a';
    }
    CHECK(!read_name_exact(p, 100, 0, name, sizeof name, &after),
          "a name with no root label runs out of message and is refused");

    /* the reserved bit patterns, which are not lengths and not pointers */
    memset(p, 0, sizeof p);
    p[0] = 0x40;
    CHECK(!read_name_exact(p, 4, 0, name, sizeof name, &after),
          "0x40 is reserved and is not read as a length");
    p[0] = 0x80;
    CHECK(!read_name_exact(p, 4, 0, name, sizeof name, &after),
          "and neither is 0x80");

    /* a name longer than the caller's buffer */
    memset(p, 0, sizeof p);
    at = put_name(p, 0, "abcdefgh.abcdefgh.abcdefgh");
    char small[10];
    CHECK(!read_name_exact(p, at, 0, small, sizeof small, &after),
          "a name too long for the buffer is refused rather than cut, "
          "a truncated name resolves to something, and something is what "
          "gets connected to");

    /* the overflow has to land on the *last* label to mean anything. */
    memset(p, 0, sizeof p);
    at = put_name(p, 0, "abc.defghijkl");
    char tight[10];
    CHECK(!read_name_exact(p, at, 0, tight, sizeof tight, &after),
          "and one whose *final* label runs over the end of the buffer, "
          "where nothing later would notice");



    n = good_reply(p, 0x2222, "example.com", IPV4(93, 184, 216, 34));
    CHECK(dns_parse_response(p, n, 0x2222, "example.com", &got, &ttl) == DNS_OK,
          "an ordinary reply is read");
    CHECK(got == IPV4(93, 184, 216, 34), "with the address in it");
    CHECK(ttl == 300, "and the ttl");

    /* the case a server sends back need not be the case that was asked */
    n = good_reply(p, 0x2223, "EXAMPLE.com", IPV4(1, 2, 3, 4));
    CHECK(dns_parse_response(p, n, 0x2223, "example.com", &got, &ttl) == DNS_OK,
          "a name is case-insensitive, a server may echo the question "
          "back in any case, and some vary it on purpose");



    n = good_reply(p, 0x3333, "example.com", IPV4(1, 2, 3, 4));

    CHECK(dns_parse_response(p, n, 0x9999, "example.com", &got, &ttl)
              == DNS_MALFORMED,
          "a reply carrying somebody else's id is not this answer, "
          "which is the whole of what stops a forged one being taken");

    CHECK(dns_parse_response(p, n, 0x3333, "other.com", &got, &ttl)
              == DNS_MALFORMED,
          "and neither is one answering a different question");

    CHECK(dns_parse_response(p, 8, 0x3333, "example.com", &got, &ttl)
              == DNS_MALFORMED,
          "a reply shorter than a header is refused");

    /* a query, sent back as though it were an answer */
    n = good_reply(p, 0x3334, "example.com", IPV4(1, 2, 3, 4));
    p[2] = 0x01; p[3] = 0x00;
    CHECK(dns_parse_response(p, n, 0x3334, "example.com", &got, &ttl)
              == DNS_MALFORMED,
          "a message with the question bit set is not an answer");

    /* the rcodes worth telling apart */
    at = put_header(p, 0x4444, 0x8183, 1, 0);   /* rcode 3 */
    at = put_name(p, at, "nope.example.com");
    p[at++] = 0; p[at++] = 1; p[at++] = 0; p[at++] = 1;
    CHECK(dns_parse_response(p, at, 0x4444, "nope.example.com", &got, &ttl)
              == DNS_NO_SUCH_NAME,
          "there is no such name is an *answer*, not a failure, "
          "retrying it asks a question that has already been settled");

    at = put_header(p, 0x4445, 0x8185, 1, 0);   /* rcode 5, refused */
    at = put_name(p, at, "example.com");
    p[at++] = 0; p[at++] = 1; p[at++] = 0; p[at++] = 1;
    CHECK(dns_parse_response(p, at, 0x4445, "example.com", &got, &ttl)
              == DNS_REFUSED,
          "and a refusal is its own thing again");

    /* rcode 0, and no answers at all */
    at = put_header(p, 0x4446, 0x8180, 1, 0);
    at = put_name(p, at, "example.com");
    p[at++] = 0; p[at++] = 1; p[at++] = 0; p[at++] = 1;
    CHECK(dns_parse_response(p, at, 0x4446, "example.com", &got, &ttl)
              == DNS_NO_ADDRESS,
          "a name that exists with no address is a real state, it may "
          "hold only mail records");

    /* both of these first used one fixture: an A record with a huge rdlength. */

    /* an A record whose rdlength is not four, and does fit. */
    at = put_header(p, 0x5555, 0x8180, 1, 1);
    at = put_name(p, at, "example.com");
    p[at++] = 0; p[at++] = 1; p[at++] = 0; p[at++] = 1;
    at = put_name(p, at, "example.com");
    p[at++] = 0; p[at++] = 1;           /* type A */
    p[at++] = 0; p[at++] = 1;           /* class IN */
    p[at++] = 0; p[at++] = 0; p[at++] = 1; p[at++] = 0x2c;
    p[at++] = 0; p[at++] = 8;           /* rdlength 8, and it fits */
    for (int i = 0; i < 8; i++) {
        p[at++] = (uint8_t)(i + 1);
    }
    CHECK(parse_exact(p, at, 0x5555, "example.com", &got, &ttl)
              == DNS_MALFORMED,
          "an A record that is not four bytes is refused rather than "
          "having the first four of something else read as an address");

    /* and a record whose rdlength runs past the end of the message. */
    /*
     * one answer, not two: with a second record following, the bound is
     * caught by the *next* name-skip instead, and removing it changes
     * nothing. with one, the loop simply ends and the reply is reported
     * as "this name has no address", which is a settled answer worth
     * caching, rather than a broken reply worth asking again
     */
    at = put_header(p, 0x5556, 0x8180, 1, 1);
    at = put_name(p, at, "example.com");
    p[at++] = 0; p[at++] = 1; p[at++] = 0; p[at++] = 1;
    at = put_name(p, at, "example.com");
    p[at++] = 0; p[at++] = 5;           /* type CNAME */
    p[at++] = 0; p[at++] = 1;
    p[at++] = 0; p[at++] = 0; p[at++] = 1; p[at++] = 0x2c;
    p[at++] = 0xff; p[at++] = 0xf0;     /* claiming 65520 bytes */
    at = put_name(p, at, "real.example.com");
    CHECK(parse_exact(p, at, 0x5556, "example.com", &got, &ttl)
              == DNS_MALFORMED,
          "a record claiming more bytes than the message holds is refused "
          "rather than skipped over into whatever follows");

    /* an answer count larger than the answers actually present */
    n = good_reply(p, 0x5557, "example.com", IPV4(1, 2, 3, 4));
    p[6] = 0; p[7] = 40;
    CHECK(dns_parse_response(p, n, 0x5557, "example.com", &got, &ttl)
              != DNS_OK || got == IPV4(1, 2, 3, 4),
          "a count larger than what arrived does not read past the end");



    /*
     * the ordinary shape for an alias: the cname, then the A record for
     * what it points at, in one reply. the second record's *name* is
     * legitimately different from the question, which is why records are
     * not checked against it
     */
    at = put_header(p, 0x6666, 0x8180, 1, 2);
    at = put_name(p, at, "www.example.com");
    p[at++] = 0; p[at++] = 1; p[at++] = 0; p[at++] = 1;

    at = put_name(p, at, "www.example.com");
    p[at++] = 0; p[at++] = 5;           /* type CNAME */
    p[at++] = 0; p[at++] = 1;
    p[at++] = 0; p[at++] = 0; p[at++] = 1; p[at++] = 0x2c;
    {
        size_t rdlen_at = at;
        p[at++] = 0; p[at++] = 0;
        size_t start = at;
        at = put_name(p, at, "real.example.com");
        p[rdlen_at + 1] = (uint8_t)(at - start);
    }
    at = put_a(p, at, "real.example.com", IPV4(10, 20, 30, 40), 60);

    CHECK(dns_parse_response(p, at, 0x6666, "www.example.com", &got, &ttl)
              == DNS_OK,
          "a cname followed by its address is read");
    CHECK(got == IPV4(10, 20, 30, 40),
          "taking the address, and stepping over the alias rather than "
          "chasing it, the answer is right there in the same reply");

    if (failures == 0) printf("all good\n");
    return failures;
}

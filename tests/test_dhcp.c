// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_dhcp.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the dhcp conversation.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "net/net.h"
#include "net/dhcp.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

static const struct mac ME = {{ 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 }};

#define SERVER  IPV4(10, 0, 2, 2)
#define GIVEN   IPV4(10, 0, 2, 15)
#define MASK    IPV4(255, 255, 255, 0)
#define ROUTER  IPV4(10, 0, 2, 2)
#define DNS     IPV4(10, 0, 2, 3)



static size_t put_opt(uint8_t *p, size_t at, uint8_t code,
                      const uint8_t *v, uint8_t len)
{
    p[at] = code;
    p[at + 1] = len;
    memcpy(p + at + 2, v, len);
    return at + 2 + len;
}

static size_t put_ip(uint8_t *p, size_t at, uint8_t code, uint32_t v)
{
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16),
                     (uint8_t)(v >> 8),  (uint8_t)v };
    return put_opt(p, at, code, b, 4);
}

/* a reply, laid out at the offsets rfc 2131 section 2 names. */
static size_t reply(uint8_t *p, uint8_t type, uint32_t xid, uint32_t yours,
                    uint32_t lease, bool with_server_id)
{
    memset(p, 0, 512);
    p[0] = 2;                       /* op: bootreply */
    p[1] = 1;                       /* htype: ethernet */
    p[2] = 6;                       /* hlen */
    p[4] = (uint8_t)(xid >> 24); p[5] = (uint8_t)(xid >> 16);
    p[6] = (uint8_t)(xid >> 8);  p[7] = (uint8_t)xid;
    p[16] = (uint8_t)(yours >> 24); p[17] = (uint8_t)(yours >> 16);
    p[18] = (uint8_t)(yours >> 8);  p[19] = (uint8_t)yours;
    memcpy(p + 28, ME.b, 6);
    p[236] = 0x63; p[237] = 0x82; p[238] = 0x53; p[239] = 0x63;

    size_t at = 240;
    at = put_opt(p, at, 53, &type, 1);
    if (with_server_id) {
        at = put_ip(p, at, 54, SERVER);
    }
    at = put_ip(p, at, 1, MASK);
    at = put_ip(p, at, 3, ROUTER);
    at = put_ip(p, at, 6, DNS);
    if (lease != 0) {
        at = put_ip(p, at, 51, lease);
    }
    p[at++] = 255;
    return at;
}



/* the client's own message, read back at the rfc's offsets. */
static uint8_t sent_type(const uint8_t *p)
{
    for (size_t at = 240; at < 300; ) {
        if (p[at] == 255) { break; }
        if (p[at] == 0)   { at++; continue; }
        if (p[at] == 53)  { return p[at + 2]; }
        at += 2 + p[at + 1];
    }
    return 0;
}

static bool has_opt(const uint8_t *p, size_t len, uint8_t code, uint32_t *v)
{
    for (size_t at = 240; at + 1 < len; ) {
        if (p[at] == 255) { break; }
        if (p[at] == 0)   { at++; continue; }
        if (p[at] == code) {
            if (v != NULL && p[at + 1] == 4) {
                *v = (uint32_t)p[at + 2] << 24 | (uint32_t)p[at + 3] << 16
                   | (uint32_t)p[at + 4] << 8  | p[at + 5];
            }
            return true;
        }
        at += 2 + p[at + 1];
    }
    return false;
}

/* parse a message out of a buffer that is exactly its length. */
static bool parse_exact(const uint8_t *p, size_t len, struct dhcp_message *m)
{
    uint8_t *exact = malloc(len);
    memcpy(exact, p, len);
    bool ok = dhcp_parse(exact, len, m);
    free(exact);
    return ok;
}

static uint32_t sent_xid(const uint8_t *p)
{
    return (uint32_t)p[4] << 24 | (uint32_t)p[5] << 16
         | (uint32_t)p[6] << 8  | p[7];
}
static uint32_t sent_ciaddr(const uint8_t *p)
{
    return (uint32_t)p[12] << 24 | (uint32_t)p[13] << 16
         | (uint32_t)p[14] << 8  | p[15];
}
static bool sent_broadcast_flag(const uint8_t *p)
{
    return (p[10] & 0x80) != 0;
}

int main(void)
{
    uint8_t out[600], pkt[512];
    struct dhcp d;
    uint64_t now;
    size_t n, len;

    /*
     * the whole conversation, which is the thing that has to work
     * before any of the awkward cases are worth looking at
     */

    now = 5000;
    dhcp_start(&d, &ME, 0xdeadbeef, now);

    n = dhcp_tick(&d, now, out, sizeof out);
    CHECK(n >= DHCP_MIN_LEN, "a discover goes out at once, not after a wait");
    CHECK(sent_type(out) == DHCP_DISCOVER, "and it is a discover");
    CHECK(sent_xid(out) == 0xdeadbeef, "carrying this conversation's number");
    CHECK(sent_ciaddr(out) == 0,
          "with no address claimed, because it has none");
    CHECK(sent_broadcast_flag(out),
          "asking to be answered by broadcast, it cannot receive a "
          "unicast reply without an address");
    CHECK(!has_opt(out, n, 54, NULL),
          "and naming no server, because it does not know of one yet");

    /* nothing more until the wait is up */
    CHECK(dhcp_tick(&d, now + 500, out, sizeof out) == 0,
          "and nothing is repeated before the first wait expires");

    len = reply(pkt, DHCP_OFFER, 0xdeadbeef, GIVEN, 3600, true);
    dhcp_input(&d, pkt, len, now + 600);

    n = dhcp_tick(&d, now + 600, out, sizeof out);
    CHECK(n >= DHCP_MIN_LEN, "an offer produces a request immediately");
    CHECK(sent_type(out) == DHCP_REQUEST, "and it is a request");

    uint32_t v = 0;
    CHECK(has_opt(out, n, 50, &v) && v == GIVEN,
          "naming the offered address in option 50");
    CHECK(has_opt(out, n, 54, &v) && v == SERVER,
          "and the server that offered it, so the others know they lost");
    CHECK(sent_ciaddr(out) == 0,
          "ciaddr is still zero: the address is not this machine's until "
          "the ack, and filling it in early claims what was not granted");

    CHECK(!dhcp_take_address(&d),
          "and there is nothing to configure yet");

    len = reply(pkt, DHCP_ACK, 0xdeadbeef, GIVEN, 3600, true);
    dhcp_input(&d, pkt, len, now + 700);

    CHECK(d.state == DHCP_BOUND, "the ack binds it");
    CHECK(d.address == GIVEN && d.mask == MASK, "with the address and mask");
    CHECK(d.router == ROUTER && d.dns == DNS, "and what else was offered");
    CHECK(d.lease_s == 3600, "and the lease");
    CHECK(dhcp_take_address(&d), "the caller is told to configure it");
    CHECK(!dhcp_take_address(&d), "exactly once, not every time it asks");
    CHECK(dhcp_tick(&d, now + 800, out, sizeof out) == 0,
          "and a bound client sends nothing");

    /*
     * two machines booting at once is the ordinary case on a real wire,
     * and the xid is the only thing separating them
     */

    now = 1000;
    dhcp_start(&d, &ME, 0x11111111, now);
    dhcp_tick(&d, now, out, sizeof out);

    len = reply(pkt, DHCP_OFFER, 0x22222222, IPV4(10, 0, 2, 99), 3600, true);
    dhcp_input(&d, pkt, len, now);
    CHECK(d.state == DHCP_SELECTING,
          "an offer with another machine's xid is not an answer to this one");
    CHECK(d.address == 0, "and its address is not taken");



    /* an offer with no address in it */
    now = 1000;
    dhcp_start(&d, &ME, 0x33, now);
    dhcp_tick(&d, now, out, sizeof out);
    len = reply(pkt, DHCP_OFFER, 0x33, 0, 3600, true);
    dhcp_input(&d, pkt, len, now);
    CHECK(d.state == DHCP_SELECTING, "an offer of nothing is not an offer");

    /* a second offer after one has been taken */
    now = 1000;
    dhcp_start(&d, &ME, 0x44, now);
    dhcp_tick(&d, now, out, sizeof out);
    len = reply(pkt, DHCP_OFFER, 0x44, GIVEN, 3600, true);
    dhcp_input(&d, pkt, len, now);
    len = reply(pkt, DHCP_OFFER, 0x44, IPV4(10, 0, 2, 77), 3600, true);
    dhcp_input(&d, pkt, len, now);
    CHECK(d.address == GIVEN,
          "a second offer does not replace the one already being requested");
    CHECK(d.offers == 1, "and is not counted as one that was acted on");

    /* an ack that grants something other than what was offered */
    len = reply(pkt, DHCP_ACK, 0x44, IPV4(10, 0, 2, 88), 3600, true);
    dhcp_input(&d, pkt, len, now);
    CHECK(d.address == IPV4(10, 0, 2, 88),
          "the ack is what counts, not the offer, a server may grant "
          "something else and that is what the machine has");

    /* a nak */
    now = 1000;
    dhcp_start(&d, &ME, 0x55, now);
    dhcp_tick(&d, now, out, sizeof out);
    len = reply(pkt, DHCP_OFFER, 0x55, GIVEN, 3600, true);
    dhcp_input(&d, pkt, len, now);
    len = reply(pkt, DHCP_NAK, 0x55, 0, 0, true);
    dhcp_input(&d, pkt, len, now + 10);
    CHECK(d.state == DHCP_SELECTING, "a nak sends it back to the start");
    CHECK(d.address == 0 && d.server == 0,
          "forgetting everything it was told, because all of it is now "
          "known to be wrong");
    n = dhcp_tick(&d, now + 10, out, sizeof out);
    CHECK(n > 0 && sent_type(out) == DHCP_DISCOVER,
          "and it starts over with a discover rather than a request");

    /* an ack with no lease in it */
    now = 1000;
    dhcp_start(&d, &ME, 0x66, now);
    dhcp_tick(&d, now, out, sizeof out);
    len = reply(pkt, DHCP_OFFER, 0x66, GIVEN, 0, true);
    dhcp_input(&d, pkt, len, now);
    len = reply(pkt, DHCP_ACK, 0x66, GIVEN, 0, true);
    dhcp_input(&d, pkt, len, now);
    CHECK(d.state == DHCP_BOUND, "an ack with no lease still binds");
    CHECK(d.lease_s > 0,
          "and gets a lease anyway, zero would renew immediately and "
          "forever");

    /*
     * the backoff, which is the part that cannot be tested with a real
     * clock without sitting through it
     */

    now = 0;
    dhcp_start(&d, &ME, 0x77, now);

    unsigned sends = 0;
    uint64_t at_ms[8];
    for (now = 0; now <= 60000; now += 10) {
        if (dhcp_tick(&d, now, out, sizeof out) > 0) {
            if (sends < 8) { at_ms[sends] = now; }
            sends++;
        }
    }
    CHECK(sends == DHCP_TRIES, "it gives up after four tries");
    CHECK(d.state == DHCP_FAILED, "and says so rather than going quiet");
    CHECK(at_ms[0] == 0, "the first goes out immediately");
    CHECK(at_ms[1] == 1000, "then after a second");
    CHECK(at_ms[2] == 3000, "then two more");
    CHECK(at_ms[3] == 7000, "then four more, the wait doubles each time");
    CHECK(dhcp_tick(&d, 120000, out, sizeof out) == 0,
          "and a failed client stays quiet rather than starting again");

    /* the same, waiting for an ack that never comes */
    now = 0;
    dhcp_start(&d, &ME, 0x88, now);
    dhcp_tick(&d, now, out, sizeof out);
    len = reply(pkt, DHCP_OFFER, 0x88, GIVEN, 3600, true);
    dhcp_input(&d, pkt, len, now);

    sends = 0;
    for (now = 0; now <= 60000; now += 10) {
        if (dhcp_tick(&d, now, out, sizeof out) > 0) {
            CHECK(sent_type(out) == DHCP_REQUEST, "it repeats the request");
            sends++;
        }
    }
    CHECK(sends == DHCP_TRIES, "four times, and then gives up");
    CHECK(d.state == DHCP_FAILED, "an offer nobody will confirm also fails");



    now = 0;
    dhcp_start(&d, &ME, 0x99, now);
    dhcp_tick(&d, now, out, sizeof out);
    len = reply(pkt, DHCP_OFFER, 0x99, GIVEN, 600, true);
    dhcp_input(&d, pkt, len, now);
    dhcp_tick(&d, now, out, sizeof out);
    len = reply(pkt, DHCP_ACK, 0x99, GIVEN, 600, true);
    dhcp_input(&d, pkt, len, now);

    /* as the real caller does: configure the interface with what was granted. */
    CHECK(dhcp_take_address(&d), "the first grant is applied");

    CHECK(dhcp_lease_left(&d, 0) == 600, "the whole lease is left at first");
    CHECK(dhcp_lease_left(&d, 300000) == 300, "half of it half way through");
    CHECK(dhcp_lease_left(&d, 600000) == 0, "and none of it at the end");
    CHECK(dhcp_lease_left(&d, 900000) == 0, "and no less than none after");

    CHECK(dhcp_tick(&d, 299000, out, sizeof out) == 0,
          "nothing is sent before half the lease is gone");

    n = dhcp_tick(&d, 300000, out, sizeof out);
    CHECK(n > 0, "and a renewal goes out at exactly half");
    CHECK(sent_type(out) == DHCP_REQUEST, "which is a request");
    CHECK(sent_ciaddr(out) == GIVEN,
          "with ciaddr filled in this time, the machine has the address "
          "and is asking to keep it");
    CHECK(!sent_broadcast_flag(out),
          "and no broadcast flag, because there is now an address to "
          "reply to and the whole wire need not hear it");
    CHECK(!has_opt(out, n, 50, NULL),
          "and no option 50: ciaddr already says which lease this is");
    CHECK(d.state == DHCP_RENEWING, "the client is renewing");

    /* a renewal that is confirmed */
    len = reply(pkt, DHCP_ACK, 0x99, GIVEN, 600, true);
    dhcp_input(&d, pkt, len, 300100);
    CHECK(d.state == DHCP_BOUND, "and the ack binds it again");
    CHECK(dhcp_lease_left(&d, 300100) == 600, "with a fresh lease");
    CHECK(!dhcp_take_address(&d),
          "a renewal of the same address is not a reconfiguration, "
          "reapplying it would reset the arp cache for nothing");

    /*
     * a renewal nobody answers: it keeps trying until the lease is
     * actually gone, rather than giving up after four
     */
    now = 300100;
    dhcp_tick(&d, now + 300000, out, sizeof out);
    CHECK(d.state == DHCP_RENEWING, "renewing again at half of the new lease");

    sends = 0;
    bool went_back = false;
    for (now = 600100; now <= 1000000; now += 100) {
        if (dhcp_tick(&d, now, out, sizeof out) > 0) { sends++; }
        if (d.state == DHCP_SELECTING) { went_back = true; break; }
    }
    CHECK(sends > DHCP_TRIES,
          "a renewal keeps trying past four, it has the rest of the "
          "lease to work in and the machine is running the whole time");
    CHECK(went_back,
          "and when the lease really runs out it starts over rather than "
          "keeping an address it no longer holds");



    struct dhcp_message m;
    len = reply(pkt, DHCP_ACK, 0x99, GIVEN, 600, true);

    CHECK(!parse_exact(pkt, 100, &m), "a message cut short is refused");
    CHECK(!parse_exact(pkt, 239, &m), "even one byte into the cookie");
    CHECK(parse_exact(pkt, len, &m), "and the whole one is not");

    pkt[236] = 0;
    CHECK(!parse_exact(pkt, len, &m),
          "a message with no magic cookie is bootp, not dhcp");
    pkt[236] = 0x63;

    pkt[0] = 1;
    CHECK(!parse_exact(pkt, len, &m),
          "and a request is not a reply, this machine's own broadcast "
          "comes back to it on a wire with a hub on it");
    pkt[0] = 2;

    /* an option whose length runs off the end. */
    len = reply(pkt, DHCP_ACK, 0x99, GIVEN, 600, true);
    pkt[len - 1] = 3;           /* was the end marker */
    pkt[len] = 100;             /* claiming a hundred bytes that are not there */
    CHECK(!parse_exact(pkt, len + 1, &m),
          "an option claiming more bytes than arrived is refused rather "
          "than read");

    /*
     * and the same thing one byte short of legal, to pin the boundary
     * rather than the general idea. an option whose value ends exactly
     * at the last byte is fine
     */
    len = reply(pkt, DHCP_ACK, 0x99, GIVEN, 600, true);
    pkt[len - 1] = 3;
    pkt[len] = 4;
    pkt[len + 1] = 10; pkt[len + 2] = 0; pkt[len + 3] = 2; pkt[len + 4] = 9;
    CHECK(parse_exact(pkt, len + 5, &m) && m.router == IPV4(10, 0, 2, 9),
          "an option ending exactly at the last byte is read, not refused");
    CHECK(!parse_exact(pkt, len + 4, &m),
          "and one byte less than that is refused");

    /* an option code with no length byte after it */
    len = reply(pkt, DHCP_ACK, 0x99, GIVEN, 600, true);
    pkt[len - 1] = 53;
    CHECK(!parse_exact(pkt, len, &m),
          "and a code at the very end with no length after it");

    /* a message that is nothing but padding */
    len = reply(pkt, DHCP_ACK, 0x99, GIVEN, 600, true);
    memset(pkt + 240, 0, len - 240);
    CHECK(!parse_exact(pkt, len, &m),
          "a message with no type option is not a dhcp message");



    now = 1000;
    dhcp_start(&d, &ME, 0xaa, now);
    dhcp_tick(&d, now, out, sizeof out);
    dhcp_stop(&d);
    CHECK(dhcp_tick(&d, now + 100000, out, sizeof out) == 0,
          "a stopped client sends nothing");
    len = reply(pkt, DHCP_OFFER, 0xaa, GIVEN, 3600, true);
    dhcp_input(&d, pkt, len, now + 100000);
    CHECK(d.state == DHCP_OFF && d.address == 0,
          "and takes nothing it is offered, somebody set an address by "
          "hand, and dhcp overwriting it later is the worst kind of bug");

    /*
     * `dhcp stop` leaves the address in the struct, because it is still
     * what the machine holds until something replaces it. what it must
     * not do is keep looking like a live lease, the shell prints this
     * and printed "renewed at half" for a client that would never renew
     * again
     */
    now = 1000;
    dhcp_start(&d, &ME, 0xbb, now);
    dhcp_tick(&d, now, out, sizeof out);
    len = reply(pkt, DHCP_OFFER, 0xbb, GIVEN, 600, true);
    dhcp_input(&d, pkt, len, now);
    dhcp_tick(&d, now, out, sizeof out);
    len = reply(pkt, DHCP_ACK, 0xbb, GIVEN, 600, true);
    dhcp_input(&d, pkt, len, now);
    CHECK(d.state == DHCP_BOUND, "bound first");

    dhcp_stop(&d);
    CHECK(d.state == DHCP_OFF, "and then given up");
    CHECK(d.address == GIVEN,
          "the address is still what the machine holds, stopping the "
          "client does not take it away");
    CHECK(dhcp_tick(&d, now + 400000, out, sizeof out) == 0,
          "but half past the lease brings no renewal, which is the whole "
          "difference between this and a live one");

    if (failures == 0) printf("all good\n");
    return failures;
}

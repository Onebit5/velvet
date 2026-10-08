// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_net.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the protocols.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "net/net.h"
#include "net/ether.h"
#include "net/arp.h"
#include "net/ip.h"
#include "net/icmp.h"
#include "net/udp.h"
#include "net/socket.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{


    char text[32];
    struct mac m = { { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 } };
    mac_format(&m, text, sizeof text);
    CHECK(strcmp(text, "52:54:00:12:34:56") == 0, "a mac address prints");

    ipv4_format(IPV4(10, 0, 2, 15), text, sizeof text);
    CHECK(strcmp(text, "10.0.2.15") == 0, "and an ip address");
    ipv4_format(IPV4(255, 255, 255, 0), text, sizeof text);
    CHECK(strcmp(text, "255.255.255.0") == 0, "including three-digit parts");

    ipv4 a = 0;
    CHECK(ipv4_parse("10.0.2.2", &a) && a == IPV4(10, 0, 2, 2),
          "and one reads back");
    CHECK(ipv4_parse("0.0.0.0", &a) && a == 0, "including the empty one");
    CHECK(ipv4_parse("255.255.255.255", &a) && a == 0xffffffff,
          "and the broadcast");

    CHECK(!ipv4_parse("10.0.2", &a), "three parts is not an address");
    CHECK(!ipv4_parse("10.0.2.256", &a), "nor is a part over 255");
    CHECK(!ipv4_parse("10.0.2.2.2", &a), "nor five parts");
    CHECK(!ipv4_parse("10.0.2.2 ", &a), "nor one with rubbish after it");
    CHECK(!ipv4_parse("10..2.2", &a), "and an empty part is not a zero");
    CHECK(!ipv4_parse("", &a), "and neither is nothing at all");

    /*
     * the worked example from rfc 1071 itself, which is the only fixture
     * here that cannot possibly have been written to match its code
     */

    const uint8_t rfc1071[] = { 0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5,
                                0xf6, 0xf7 };
    CHECK(net_checksum(rfc1071, sizeof rfc1071) == 0x220d,
          "the worked example from rfc 1071 gives the answer rfc 1071 gives");

    /* a block containing its own checksum sums to zero. */
    uint8_t withsum[10];
    memcpy(withsum, rfc1071, 8);
    put16be(withsum + 8, net_checksum(rfc1071, 8));
    CHECK(net_checksum(withsum, sizeof withsum) == 0,
          "and a block carrying its own checksum sums to zero");

    /*
     * accumulating in pieces must give the same answer as one call,
     * including when a piece has an *odd* length, which is where a
     * word-at-a-time implementation quietly restarts its alignment and
     * gets a different, wrong answer
     */
    const uint8_t odd[] = { 0x45, 0x00, 0x00, 0x54, 0xab, 0xcd, 0x40 };

    struct net_sum s;
    net_sum_start(&s);
    net_sum_add(&s, odd, 3);            /* odd */
    net_sum_add(&s, odd + 3, 4);
    CHECK(net_sum_finish(&s) == net_checksum(odd, sizeof odd),
          "a checksum taken in two pieces, the first of odd length, is the "
          "same as one taken in a single call, the parity has to carry "
          "between them, and a word-at-a-time version that realigns is "
          "right for every even-sized packet and wrong for udp");

    net_sum_start(&s);
    for (size_t i = 0; i < sizeof odd; i++) {
        net_sum_add(&s, odd + i, 1);
    }
    CHECK(net_sum_finish(&s) == net_checksum(odd, sizeof odd),
          "and the same taken one byte at a time");

    /* folding the carries has to be a loop, not one pass. */
    const uint8_t carries_twice[] = { 0xff, 0xff, 0xff, 0xff, 0x00, 0x01 };
    CHECK(net_checksum(carries_twice, sizeof carries_twice) == 0xfffe,
          "a sum that carries out of the fold is folded again, one pass "
          "is right for small packets and wrong near an mtu");



    uint8_t frame[64];
    struct mac to = { { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff } };
    size_t at = ether_build(frame, &to, &m, ETHERTYPE_ARP);
    CHECK(at == ETHER_HEADER_LEN, "an ethernet header is fourteen bytes");

    struct ether_header eh;
    CHECK(ether_parse(frame, sizeof frame, &eh), "and reads back");
    CHECK(mac_is_broadcast(&eh.to), "to the broadcast address");
    CHECK(mac_equal(&eh.from, &m), "from this machine");
    CHECK(eh.type == ETHERTYPE_ARP, "carrying arp");
    CHECK(!ether_parse(frame, 13, &eh), "and thirteen bytes is not a frame");

    /* qemu's gateway asking who has 10.0.2.15. */

    const uint8_t real_arp[28] = {
        0x00, 0x01,                             /* ethernet */
        0x08, 0x00,                             /* ipv4 */
        0x06, 0x04,                             /* 6-byte, 4-byte addresses */
        0x00, 0x01,                             /* request */
        0x52, 0x55, 0x0a, 0x00, 0x02, 0x02,     /* sender mac */
        0x0a, 0x00, 0x02, 0x02,                 /* sender ip 10.0.2.2 */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     /* target mac, unknown */
        0x0a, 0x00, 0x02, 0x0f,                 /* target ip 10.0.2.15 */
    };

    struct arp_packet ap;
    CHECK(arp_parse(real_arp, sizeof real_arp, &ap),
          "an arp request laid out from the rfc parses");
    CHECK(ap.op == ARP_REQUEST, "as a request");
    CHECK(ap.sender_ip == IPV4(10, 0, 2, 2), "from 10.0.2.2");
    CHECK(ap.target_ip == IPV4(10, 0, 2, 15), "asking about 10.0.2.15");
    CHECK(ap.sender_mac.b[0] == 0x52 && ap.sender_mac.b[5] == 0x02,
          "with the sender's hardware address");

    /* and what this machine would build must be byte-identical to it */
    uint8_t built[28];
    struct mac sender = { { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 } };
    size_t n = arp_build(built, ARP_REQUEST, &sender, IPV4(10, 0, 2, 2),
                         &MAC_ZERO, IPV4(10, 0, 2, 15));
    CHECK(n == ARP_PACKET_LEN, "an arp packet is 28 bytes");
    CHECK(memcmp(built, real_arp, 28) == 0,
          "and what this machine builds is byte for byte what the layout "
          "says it should be, the check that catches a field in the wrong "
          "place, since its own parser would happily read its own mistake");

    /* what it must refuse */
    uint8_t bad[28];
    memcpy(bad, real_arp, 28);
    put16be(bad, 6);                            /* token ring */
    CHECK(!arp_parse(bad, 28, &ap), "arp for another kind of wire is refused");
    memcpy(bad, real_arp, 28);
    bad[4] = 8;                                 /* eight-byte addresses */
    CHECK(!arp_parse(bad, 28, &ap), "and so is arp with the wrong widths");
    CHECK(!arp_parse(real_arp, 27, &ap), "and a packet one byte short");



    struct arp_cache cache;
    arp_cache_reset(&cache);
    struct mac found;

    CHECK(!arp_lookup(&cache, IPV4(10, 0, 2, 2), 1000, &found),
          "an empty cache knows nobody");

    arp_learn(&cache, IPV4(10, 0, 2, 2), &sender, 1000);
    CHECK(arp_lookup(&cache, IPV4(10, 0, 2, 2), 1000, &found),
          "and then it does");
    CHECK(mac_equal(&found, &sender), "with the right hardware address");
    CHECK(arp_count(&cache, 1000) == 1, "and one entry to show");

    /* the newest claim wins. */
    struct mac impostor = { { 0xde, 0xad, 0xbe, 0xef, 0x00, 0x01 } };
    arp_learn(&cache, IPV4(10, 0, 2, 2), &impostor, 2000);
    CHECK(arp_lookup(&cache, IPV4(10, 0, 2, 2), 2000, &found)
          && mac_equal(&found, &impostor),
          "a later answer replaces an earlier one rather than being a "
          "second entry for the same address");
    CHECK(arp_count(&cache, 2000) == 1, "and there is still only one");

    CHECK(!arp_lookup(&cache, IPV4(10, 0, 2, 2), 2000 + ARP_TTL_MS + 1, &found),
          "an answer goes stale, because machines move and cards get "
          "swapped and an address that was right an hour ago is a packet "
          "sent into nowhere");
    CHECK(arp_count(&cache, 2000 + ARP_TTL_MS + 1) == 0,
          "and a stale entry is not listed either");

    /* filling it must not lose the ability to learn */
    arp_cache_reset(&cache);
    for (unsigned i = 0; i < ARP_CACHE_MAX + 4; i++) {
        struct mac each = { { 0x02, 0, 0, 0, 0, (uint8_t)i } };
        arp_learn(&cache, IPV4(10, 0, 0, i), &each, 1000 + i);
    }
    CHECK(arp_count(&cache, 1100) == ARP_CACHE_MAX, "a full cache stays full");
    CHECK(arp_lookup(&cache, IPV4(10, 0, 0, ARP_CACHE_MAX + 3), 1100, &found),
          "and the newest is still in it, a cache that dropped the new "
          "answer instead of the stalest would never learn anything again");

    /* the front of an ordinary ping. */

    static uint8_t real_ip[84] = {
        0x45, 0x00, 0x00, 0x54,                 /* v4, 20 bytes, total 84 */
        0xa6, 0xf2, 0x40, 0x00,                 /* id, don't fragment */
        0x40, 0x01, 0x7b, 0xa6,                 /* ttl 64, icmp, checksum */
        0x0a, 0x00, 0x02, 0x0f,                 /* 10.0.2.15 */
        0x0a, 0x00, 0x02, 0x02,                 /* 10.0.2.2 */
        /*
         * and 64 bytes of payload, which the header's checksum does not
         * cover and the length field does count
         */
    };

    struct ip_header ih;
    CHECK(net_checksum(real_ip, 20) == 0,
          "the fixture's own checksum is right, by the receiver's test");
    CHECK(ip_parse(real_ip, sizeof real_ip, &ih),
          "an ip datagram parses, checksum and all");
    CHECK(ih.header_len == 20, "twenty bytes of header");
    CHECK(ih.total_len == 84, "eighty-four altogether");
    CHECK(ih.protocol == IP_PROTO_ICMP, "carrying icmp");
    CHECK(ih.ttl == 64, "with the conventional ttl");
    CHECK(ih.from == IPV4(10, 0, 2, 15) && ih.to == IPV4(10, 0, 2, 2),
          "between the right two addresses");
    CHECK(ih.payload_at == 20 && ih.payload_len == 64,
          "and the payload is what is left after the header");

    /* one bit flipped anywhere in it must be refused */
    static uint8_t badip[84];
    memcpy(badip, real_ip, 84);
    badip[8] ^= 0x01;                           /* the ttl */
    CHECK(!ip_parse(badip, 84, &ih),
          "a header with one bit changed fails its checksum");

    memcpy(badip, real_ip, 84);
    put16be(badip + 2, 200);                    /* claims more than arrived */
    put16be(badip + 10, 0);
    put16be(badip + 10, net_checksum(badip, 20));
    CHECK(!ip_parse(badip, 84, &ih),
          "a total length longer than the frame is refused even with a "
          "good checksum, believing it means reading bytes nobody sent");

    memcpy(badip, real_ip, 84);
    badip[0] = 0x65;                            /* version 6 */
    CHECK(!ip_parse(badip, 84, &ih), "and so is anything not version 4");

    memcpy(badip, real_ip, 84);
    put16be(badip + 6, 0x2000);                 /* more fragments */
    put16be(badip + 10, 0);
    put16be(badip + 10, net_checksum(badip, 20));
    CHECK(!ip_parse(badip, 84, &ih),
          "and a fragment is dropped rather than read as a whole datagram, "
          "which is how a stack ends up acting on half a packet");

    /* what this machine builds must read back as what it meant */
    static uint8_t out[84];
    ip_build(out, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2), IP_PROTO_ICMP, 64,
             0xa6f2);
    CHECK(ip_parse(out, sizeof out, &ih), "a header this machine built parses");
    CHECK(memcmp(out, real_ip, 20) == 0,
          "and comes out byte for byte the same as the one laid out from "
          "the rfc, checksum included");
    CHECK(net_checksum(out, 20) == 0,
          "and its checksum is right by the receiver's own test");
    CHECK(ih.total_len == 84,
          "and total_len counts the header too, which is the field most "
          "often set to just the payload");



    uint8_t echo[72];
    const char *payload = "abcdefghijklmnopqrstuvwxyz";
    size_t elen = icmp_build_echo(echo, ICMP_ECHO_REQUEST, 0x1234, 1,
                                  payload, strlen(payload));
    CHECK(elen == ICMP_HEADER_LEN + strlen(payload), "an echo is header plus payload");

    struct icmp_echo ie;
    CHECK(icmp_parse_echo(echo, elen, &ie), "and parses back");
    CHECK(ie.type == ICMP_ECHO_REQUEST && ie.id == 0x1234 && ie.seq == 1,
          "with its type, id and sequence");
    CHECK(memcmp(echo + ie.payload_at, payload, ie.payload_len) == 0,
          "and the payload untouched, which is the whole of what ping "
          "measures");
    CHECK(net_checksum(echo, elen) == 0,
          "the icmp checksum covers the payload as well as the header, "
          "unlike ip's, which covers only the header");

    echo[20] ^= 0x40;                           /* a byte of the payload */
    CHECK(!icmp_parse_echo(echo, elen, &ie),
          "so changing a payload byte fails it");

    /*
     * the pseudo-header is the interesting part: twelve bytes that are
     * never sent, holding the addresses the datagram travelled between.
     * a stack that checksums only the datagram produces something every
     * other machine discards, and, worse, *accepts* datagrams that
     * were delivered to the wrong host, which is precisely what the
     * pseudo-header exists to prevent
     */

    static uint8_t dgram[64];
    const char *hello = "hee-ho";
    size_t ulen = udp_build(dgram, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2),
                            5000, 7, hello, strlen(hello));
    CHECK(ulen == UDP_HEADER_LEN + strlen(hello),
          "a udp datagram is eight bytes and a payload");
    CHECK(get16be(dgram + 4) == ulen,
          "and its length field counts the header too");

    struct udp_header uh;
    CHECK(udp_parse(dgram, ulen, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2), &uh),
          "it parses back, checksum and all");
    CHECK(uh.from_port == 5000 && uh.to_port == 7, "with its ports");
    CHECK(uh.payload_len == strlen(hello)
          && memcmp(dgram + uh.payload_at, hello, uh.payload_len) == 0,
          "and its payload");

    /* the same bytes, checked against different addresses, must fail. */
    CHECK(!udp_parse(dgram, ulen, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 3), &uh),
          "and the same datagram checked against a different destination "
          "fails, the checksum covers addresses that are not in it, so a "
          "datagram delivered to the wrong host is refused rather than "
          "accepted by whoever happened to receive it");
    CHECK(!udp_parse(dgram, ulen, IPV4(10, 0, 2, 9), IPV4(10, 0, 2, 2), &uh),
          "and the same for a different source");

    /*
     * an odd-length payload is where a word-at-a-time checksum that
     * restarts its alignment between the pseudo-header and the datagram
     * gets a different answer
     */
    ulen = udp_build(dgram, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2), 5000, 7,
                     "odd", 3);
    CHECK(udp_parse(dgram, ulen, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2), &uh),
          "a payload of odd length checksums correctly, which needs the "
          "sum to carry its parity from the pseudo-header into the "
          "datagram rather than realigning between them");

    /* a sender that did not compute one */
    put16be(dgram + 6, 0);
    CHECK(udp_parse(dgram, ulen, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2), &uh),
          "a checksum of zero means the sender did not compute one, which "
          "udp allows and ip does not, refusing it would drop legal "
          "traffic");

    /* a payload whose checksum comes out as exactly zero. */
    const uint8_t makes_zero[2] = { 0xd4, 0x3a };
    ulen = udp_build(dgram, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2), 5000, 7,
                     makes_zero, sizeof makes_zero);
    CHECK(get16be(dgram + 6) == 0xffff,
          "a checksum that computes to zero goes out as all ones instead, "
          "because zero on the wire means `not computed`, the two are the "
          "same number in ones-complement, which is the only reason the "
          "convention works at all");
    CHECK(udp_parse(dgram, ulen, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2), &uh),
          "and it still verifies");

    /* a length longer than arrived, with no checksum to catch it. */
    ulen = udp_build(dgram, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2), 5000, 7,
                     hello, strlen(hello));
    put16be(dgram + 4, 200);
    put16be(dgram + 6, 0);
    CHECK(!udp_parse(dgram, ulen, IPV4(10, 0, 2, 15), IPV4(10, 0, 2, 2), &uh),
          "a length longer than the datagram is refused on its own merits, "
          "before any checksum is taken, otherwise the sum itself is what "
          "reads past the end");

    /* the queue is the whole design. */

    static struct socket_table st;
    socket_table_reset(&st);

    int h = socket_open(&st, 7, 5000);
    CHECK(h >= 0, "a port can be claimed");
    CHECK(socket_open(&st, 7, 5000) < 0,
          "and not claimed twice, two programs on one port means a "
          "datagram going to whichever was found first, which is worse "
          "than refusing");
    CHECK(socket_open(&st, 9, 5000) < 0,
          "not even by somebody else");
    CHECK(socket_open(&st, 7, 0) < 0, "and port zero is nobody's");

    uint16_t bound = 0;
    CHECK(socket_port(&st, 7, h, &bound) && bound == 5000,
          "a handle knows which port it is");
    CHECK(!socket_port(&st, 9, h, &bound),
          "and another program cannot ask about it");

    /* delivery */
    ipv4 who = 0; uint16_t whoport = 0;
    uint8_t buf[64];

    CHECK(socket_take(&st, 7, h, &who, &whoport, buf, sizeof buf) == -1,
          "an empty socket says `not yet` rather than failing");

    CHECK(socket_deliver(&st, 5000, IPV4(10, 0, 2, 2), 1234, "hee-ho", 6) >= 0,
          "a datagram for a bound port is taken");
    CHECK(socket_deliver(&st, 5001, IPV4(10, 0, 2, 2), 1234, "x", 1) < 0,
          "and one for a port nobody has is not, which is ordinary rather "
          "than an error");

    int64_t got = socket_take(&st, 7, h, &who, &whoport, buf, sizeof buf);
    CHECK(got == 6 && memcmp(buf, "hee-ho", 6) == 0, "and comes back whole");
    CHECK(who == IPV4(10, 0, 2, 2) && whoport == 1234,
          "with the sender's address beside it, one socket hears from "
          "everybody, which is what makes a udp server one port rather "
          "than a connection per talker");

    CHECK(socket_take(&st, 7, h, &who, &whoport, buf, sizeof buf) == -1,
          "and then there is nothing again");

    /* order, and the ring wrapping */
    for (int i = 0; i < SOCKET_QUEUE; i++) {
        uint8_t one = (uint8_t)i;
        CHECK(socket_deliver(&st, 5000, IPV4(10, 0, 2, 2), 1234, &one, 1) >= 0,
              "the queue fills");
    }
    CHECK(socket_waiting(&st, 7, h) == SOCKET_QUEUE, "and says how full");

    uint8_t extra = 0xff;
    CHECK(socket_deliver(&st, 5000, IPV4(10, 0, 2, 2), 1234, &extra, 1) >= 0,
          "a datagram arriving at a full queue is still `delivered`");
    CHECK(st.s[h].dropped == 1,
          "but it is dropped and counted, udp has never promised "
          "delivery, and a stack that grew a buffer instead would let "
          "anybody on the wire allocate until this machine died");

    for (int i = 0; i < SOCKET_QUEUE; i++) {
        got = socket_take(&st, 7, h, &who, &whoport, buf, sizeof buf);
        CHECK(got == 1 && buf[0] == (uint8_t)i,
              "and they come back oldest first, which a ring gets wrong "
              "by one in either direction");
    }

    /* a datagram longer than the buffer asked for */
    static uint8_t big[SOCKET_DATAGRAM];
    memset(big, 0x5a, sizeof big);
    socket_deliver(&st, 5000, IPV4(10, 0, 2, 2), 1234, big, sizeof big);

    got = socket_take(&st, 7, h, &who, &whoport, buf, 16);
    CHECK(got == 16, "a datagram longer than the buffer is cut to fit");
    CHECK(socket_take(&st, 7, h, &who, &whoport, buf, sizeof buf) == -1,
          "and the rest of it is gone rather than held back, which is how "
          "recvfrom has always behaved");

    /* ownership */
    CHECK(socket_take(&st, 9, h, &who, &whoport, buf, sizeof buf) == -1,
          "another program cannot read this socket");
    CHECK(!socket_close(&st, 9, h), "nor close it");
    CHECK(socket_close(&st, 7, h), "the owner can");
    CHECK(socket_open(&st, 7, 5000) >= 0, "and the port is free again");

    /* and everything a program held goes when it does */
    socket_table_reset(&st);
    for (int i = 0; i < 3; i++) {
        CHECK(socket_open(&st, 7, (uint16_t)(6000 + i)) >= 0, "three ports");
    }
    CHECK(socket_open(&st, 9, 7000) >= 0, "and one belonging to somebody else");
    socket_close_all(&st, 7);
    CHECK(socket_open(&st, 8, 6000) >= 0 && socket_open(&st, 8, 6001) >= 0,
          "a dead program's ports are free again");
    CHECK(socket_open(&st, 8, 7000) < 0,
          "and the other program still has its own");

    /* the table filling */
    socket_table_reset(&st);
    for (int i = 0; i < SOCKET_MAX; i++) {
        CHECK(socket_open(&st, 7, (uint16_t)(9000 + i)) >= 0, "the table fills");
    }
    CHECK(socket_open(&st, 7, 9999) < 0, "and then refuses rather than reusing");


    /*
     * this is an index rather than a yes/no, because there is now
     * somebody to wake and the caller has to know whose queue to knock
     * on. the trap is that socket 0 answers `0`, which every existing
     * `CHECK(socket_deliver(...))` read as a failure, the return value
     * changed meaning without changing type in a way the compiler could
     * see
     */
    socket_table_reset(&st);
    int first = socket_open(&st, 7, 6100);
    int second = socket_open(&st, 7, 6200);
    CHECK(first == 0 && second == 1, "two sockets, at nought and one");

    CHECK(socket_deliver(&st, 6100, IPV4(10, 0, 2, 2), 1, "a", 1) == first,
          "a datagram names the socket that took it, even when that is "
          "socket nought");
    CHECK(socket_deliver(&st, 6200, IPV4(10, 0, 2, 2), 1, "b", 1) == second,
          "and the other one");
    CHECK(socket_deliver(&st, 6300, IPV4(10, 0, 2, 2), 1, "c", 1) < 0,
          "and a port nobody holds names none of them");

    /* a full queue still names the socket. */
    for (int i = 0; i < SOCKET_QUEUE; i++) {
        socket_deliver(&st, 6100, IPV4(10, 0, 2, 2), 1, "x", 1);
    }
    CHECK(socket_deliver(&st, 6100, IPV4(10, 0, 2, 2), 1, "y", 1) == first,
          "a socket whose queue is full is still the socket it was for");

    /*
     * `net_socket_ready` walks a caller's handles asking each whether
     * anything is waiting, and parks the thread when none of them has
     * anything. the parking half needs a scheduler and is not here; the
     * *asking* half is socket_waiting, and getting that wrong is a
     * server that sleeps through its own traffic
     */
    socket_table_reset(&st);
    int one = socket_open(&st, 7, 7100);
    int two = socket_open(&st, 7, 7200);
    int mine = socket_open(&st, 9, 7300);

    CHECK(socket_waiting(&st, 7, one) == 0, "an idle socket has nothing");
    CHECK(socket_waiting(&st, 7, two) == 0, "and so does the other");

    socket_deliver(&st, 7200, IPV4(10, 0, 2, 2), 1, "x", 1);
    CHECK(socket_waiting(&st, 7, one) == 0,
          "a datagram for one socket does not make the other ready, a "
          "server woken for the wrong one would answer nobody");
    CHECK(socket_waiting(&st, 7, two) == 1, "and does make its own ready");

    socket_deliver(&st, 7200, IPV4(10, 0, 2, 2), 1, "y", 1);
    CHECK(socket_waiting(&st, 7, two) == 2, "two waiting is two, not one");

    /* somebody else's socket is not ready for the test, whatever is in it */
    socket_deliver(&st, 7300, IPV4(10, 0, 2, 2), 1, "z", 1);
    CHECK(socket_waiting(&st, 7, mine) == 0,
          "a socket belonging to another program is never ready for this "
          "one, however full it is");
    CHECK(socket_waiting(&st, 9, mine) == 1, "and is for its owner");

    /* a handle nobody holds. */
    CHECK(socket_waiting(&st, 7, 999) == 0, "a handle out of range is quiet");
    CHECK(socket_waiting(&st, 7, -1) == 0, "and so is a negative one");

    /*
     * the ethernet address and the ip address answer different
     * questions, and a frame addressed to the gateway carrying a
     * datagram for somewhere else is the ordinary case rather than a
     * contradiction. getting this backwards means arping for a machine
     * on the other side of the world, which nothing on this wire will
     * ever answer
     */
    {
        ipv4 me   = IPV4(10, 0, 2, 15);
        ipv4 mask = IPV4(255, 255, 255, 0);
        ipv4 gw   = IPV4(10, 0, 2, 2);

        CHECK(ip_next_hop(IPV4(10, 0, 2, 99), me, mask, gw)
                  == IPV4(10, 0, 2, 99),
              "a machine on this wire is asked for directly");
        CHECK(ip_next_hop(gw, me, mask, gw) == gw,
              "and so is the gateway itself, which is on it");

        CHECK(ip_next_hop(IPV4(104, 20, 23, 154), me, mask, gw) == gw,
              "anything off this wire goes to the gateway, the frame is "
              "addressed to it, and the datagram still names where it is "
              "really going");

        CHECK(ip_next_hop(IPV4(104, 20, 23, 154), me, mask, 0) == 0,
              "and with no gateway there is nowhere to send it, which is "
              "an answer rather than a guess");

        CHECK(ip_next_hop(0xffffffff, me, mask, gw) == 0,
              "a broadcast is never handed to a gateway, forwarding "
              "`everybody` is how a storm starts");

        /*
         * a different mask changes what counts as this wire, which is
         * the whole reason the mask is an argument
         */
        CHECK(ip_next_hop(IPV4(10, 0, 3, 1), me, IPV4(255, 255, 0, 0), gw)
                  == IPV4(10, 0, 3, 1),
              "a wider netmask makes more of the world local");
        CHECK(ip_next_hop(IPV4(10, 0, 3, 1), me, mask, gw) == gw,
              "and a narrower one sends the same address to the gateway");
    }

    if (failures == 0) printf("all good\n");
    return failures;
}

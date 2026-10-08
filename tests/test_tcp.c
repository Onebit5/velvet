// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_tcp.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for tcp.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "net/net.h"
#include "net/tcp.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

#define US    IPV4(10, 0, 2, 15)
#define THEM  IPV4(10, 0, 2, 2)
#define OURP  40000
#define THEIRP 80

/* the project's initial sequence number, and theirs. */
#define ISS   1000000
#define PEER  7000000

/* the ones-complement sum, written out here rather than called from net.c. */
static uint16_t sum16(const uint8_t *p, size_t len, uint32_t acc)
{
    for (size_t i = 0; i + 1 < len; i += 2) {
        acc += ((uint32_t)p[i] << 8) | p[i + 1];
    }
    if (len & 1) {
        acc += (uint32_t)p[len - 1] << 8;
    }
    while (acc >> 16) {
        acc = (acc & 0xffff) + (acc >> 16);
    }
    return (uint16_t)~acc;
}

/* build a segment the way the peer would, at rfc 793's offsets */
static size_t peer_segment(uint8_t *p, uint32_t seq, uint32_t ack,
                           uint8_t flags, uint16_t window,
                           const char *data, size_t data_len)
{
    memset(p, 0, 20);
    p[0] = (THEIRP >> 8); p[1] = THEIRP & 0xff;
    p[2] = (OURP >> 8);   p[3] = OURP & 0xff;
    p[4] = (uint8_t)(seq >> 24); p[5] = (uint8_t)(seq >> 16);
    p[6] = (uint8_t)(seq >> 8);  p[7] = (uint8_t)seq;
    p[8] = (uint8_t)(ack >> 24); p[9] = (uint8_t)(ack >> 16);
    p[10] = (uint8_t)(ack >> 8); p[11] = (uint8_t)ack;
    p[12] = 5 << 4;
    p[13] = flags;
    p[14] = (uint8_t)(window >> 8); p[15] = (uint8_t)window;

    if (data_len > 0) {
        memcpy(p + 20, data, data_len);
    }
    size_t len = 20 + data_len;

    /* the pseudo-header, summed in with the segment */
    uint8_t ph[12];
    ph[0] = 10; ph[1] = 0; ph[2] = 2; ph[3] = 2;        /* THEM */
    ph[4] = 10; ph[5] = 0; ph[6] = 2; ph[7] = 15;       /* US */
    ph[8] = 0; ph[9] = 6;
    ph[10] = (uint8_t)(len >> 8); ph[11] = (uint8_t)len;

    uint32_t acc = 0;
    for (int i = 0; i < 12; i += 2) {
        acc += ((uint32_t)ph[i] << 8) | ph[i + 1];
    }
    uint16_t ck = sum16(p, len, acc);
    p[16] = (uint8_t)(ck >> 8); p[17] = (uint8_t)ck;
    return len;
}

/* hand one to the connection, through the real parser */
static bool feed(struct tcp_conn *c, uint32_t seq, uint32_t ack,
                 uint8_t flags, uint16_t window,
                 const char *data, size_t data_len, uint64_t now)
{
    /*
     * big enough for a full-sized segment: the receive-buffer tests
     * hand this mss-sized lumps the way a real sender would, and 600
     * bytes was enough only for the small fixtures above it
     */
    uint8_t p[TCP_MSS + 64];
    size_t len = peer_segment(p, seq, ack, flags, window, data, data_len);

    struct tcp_segment seg;
    if (!tcp_parse(p, len, THEM, US, &seg)) {
        return false;
    }
    tcp_input(c, &seg, p + seg.payload_at, now);
    return true;
}

/* take whatever the connection wants to send, parsed back */
static bool pull(struct tcp_conn *c, uint64_t now, struct tcp_segment *seg,
                 uint8_t *payload_out, size_t *payload_len)
{
    static uint8_t p[2000];
    size_t n = tcp_tick(c, now, p, sizeof p);
    if (n == 0) {
        return false;
    }
    if (!tcp_parse(p, n, US, THEM, seg)) {
        printf("FAIL: the stack built a segment its own parser refuses\n");
        failures++;
        return false;
    }
    if (payload_out != NULL) {
        memcpy(payload_out, p + seg->payload_at, seg->payload_len);
        *payload_len = seg->payload_len;
    }
    return true;
}

/*
 * get a connection to ESTABLISHED, since almost everything below needs
 * one and the handshake is tested on its own first
 */
static void establish(struct tcp_conn *c, uint64_t now)
{
    struct tcp_segment seg;
    tcp_connect(c, US, OURP, THEM, THEIRP, ISS, now);
    pull(c, now, &seg, NULL, NULL);                     /* syn */
    feed(c, PEER, ISS + 1, TCP_SYN | TCP_ACK, 4096, NULL, 0, now);
    pull(c, now, &seg, NULL, NULL);                     /* the ack */
}

int main(void)
{
    struct tcp_conn c;
    struct tcp_segment seg;
    uint8_t data[2000];
    size_t data_len;
    uint64_t now = 10000;



    tcp_connect(&c, US, OURP, THEM, THEIRP, ISS, now);
    CHECK(c.state == TCP_SYN_SENT, "connect sends a syn");

    CHECK(pull(&c, now, &seg, NULL, NULL), "and there is one to send");
    CHECK(seg.flags == TCP_SYN, "with syn and nothing else, not ack: "
          "there is nothing to acknowledge yet");
    CHECK(seg.seq == ISS, "carrying the initial sequence number");
    CHECK(seg.from_port == OURP && seg.to_port == THEIRP, "and the ports");
    CHECK(!pull(&c, now, &seg, NULL, NULL), "and it is not repeated at once");

    /* a syn+ack acknowledging something else is not this connection's */
    feed(&c, PEER, ISS + 99, TCP_SYN | TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_SYN_SENT,
          "a syn+ack acknowledging what was never sent is ignored");

    feed(&c, PEER, ISS + 1, TCP_SYN | TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_ESTABLISHED, "the right one establishes it");

    CHECK(pull(&c, now, &seg, NULL, NULL), "and the third of the three");
    CHECK(seg.flags == TCP_ACK, "is a bare ack");
    CHECK(seg.seq == ISS + 1, "one past the syn, which occupied a number");
    CHECK(seg.ack == PEER + 1, "acknowledging theirs, which did too");
    CHECK(!pull(&c, now, &seg, NULL, NULL), "and then there is nothing to say");



    tcp_listen(&c, US, THEIRP, ISS);
    c.remote = THEM;                /* the caller fills this in */
    CHECK(c.state == TCP_LISTEN, "a listening end waits");
    CHECK(!pull(&c, now, &seg, NULL, NULL), "and says nothing unprompted");

    feed(&c, PEER, 0, TCP_SYN, 4096, NULL, 0, now);
    CHECK(c.state == TCP_SYN_RECEIVED, "a syn starts a connection");
    CHECK(pull(&c, now, &seg, NULL, NULL), "and is answered");
    CHECK(seg.flags == (TCP_SYN | TCP_ACK), "with syn and ack together");
    CHECK(seg.seq == ISS, "the initial number");
    CHECK(seg.ack == PEER + 1, "and theirs, acknowledged");

    feed(&c, PEER + 1, ISS + 1, TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_ESTABLISHED, "and their ack completes it");



    establish(&c, now);
    CHECK(tcp_write(&c, "hello", 5) == 5, "five bytes are accepted");

    CHECK(pull(&c, now, &seg, data, &data_len), "and go out");
    CHECK(seg.payload_len == 5 && memcmp(data, "hello", 5) == 0,
          "with the bytes in them");
    CHECK(seg.seq == ISS + 1, "at the sequence number after the syn");
    CHECK((seg.flags & TCP_ACK) != 0, "and every segment after the syn "
          "carries an acknowledgement, there is no reason not to");
    CHECK(!pull(&c, now, &seg, NULL, NULL),
          "and nothing more until it is acknowledged or times out");

    feed(&c, PEER + 1, ISS + 6, TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.send_len == 0,
          "an acknowledgement lets the buffer go, until then it is kept, "
          "because the other end may ask again");
    CHECK(tcp_writable(&c) == TCP_SEND_BUF, "and the room comes back");

    /* an acknowledgement carrying no data must not provoke another one. */
    while (pull(&c, now, &seg, NULL, NULL)) { }
    feed(&c, PEER + 1, ISS + 6, TCP_ACK, 4096, NULL, 0, now);
    CHECK(!pull(&c, now, &seg, NULL, NULL),
          "a bare acknowledgement is not answered with another one");

    feed(&c, PEER + 1, ISS + 6, TCP_ACK, 4096, NULL, 0, now);
    feed(&c, PEER + 1, ISS + 6, TCP_ACK, 4096, NULL, 0, now);
    CHECK(!pull(&c, now, &seg, NULL, NULL),
          "and neither is a repeat of one, which is what would keep the "
          "exchange going forever");

    /* an acknowledgement of what was never sent changes nothing */
    uint32_t was = c.snd_una;
    feed(&c, PEER + 1, ISS + 9999, TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.snd_una == was, "an ack of what was never sent is ignored");



    establish(&c, now);
    feed(&c, PEER + 1, ISS + 1, TCP_ACK | TCP_PSH, 4096, "world", 5, now);
    CHECK(tcp_readable(&c) == 5, "five bytes arrive");

    char got[16] = { 0 };
    CHECK(tcp_read(&c, got, sizeof got) == 5 && memcmp(got, "world", 5) == 0,
          "and read back");
    CHECK(tcp_readable(&c) == 0, "leaving nothing behind");

    CHECK(pull(&c, now, &seg, NULL, NULL), "and they are acknowledged");
    CHECK(seg.ack == PEER + 6, "up to the end of what arrived");

    /* in two pieces, which is the whole point of a stream */
    establish(&c, now);
    feed(&c, PEER + 1, ISS + 1, TCP_ACK, 4096, "abc", 3, now);
    feed(&c, PEER + 4, ISS + 1, TCP_ACK, 4096, "def", 3, now);
    CHECK(tcp_readable(&c) == 6, "two segments make six bytes");
    memset(got, 0, sizeof got);
    tcp_read(&c, got, sizeof got);
    CHECK(memcmp(got, "abcdef", 6) == 0, "in the order they were sent");

    /*
     * out of order: dropped, and the gap reported by acknowledging what
     * this end actually wants
     */
    establish(&c, now);
    feed(&c, PEER + 1, ISS + 1, TCP_ACK, 4096, "abc", 3, now);
    feed(&c, PEER + 100, ISS + 1, TCP_ACK, 4096, "zzz", 3, now);
    CHECK(tcp_readable(&c) == 3,
          "a segment ahead of a gap is dropped rather than held");
    CHECK(pull(&c, now, &seg, NULL, NULL), "and answered");
    CHECK(seg.ack == PEER + 4,
          "with the number this end wants next, which is how the other "
          "end learns something went missing");

    /* a repeat of what has already been taken */
    establish(&c, now);
    feed(&c, PEER + 1, ISS + 1, TCP_ACK, 4096, "abc", 3, now);
    tcp_read(&c, got, sizeof got);
    feed(&c, PEER + 1, ISS + 1, TCP_ACK, 4096, "abc", 3, now);
    CHECK(tcp_readable(&c) == 0,
          "a repeat of what was already taken is not delivered twice");



    establish(&c, now);
    for (int i = 0; i < 100; i++) {
        tcp_write(&c, "0123456789", 10);
    }
    /* the peer said 4096 in establish(); re-establish with a small one */
    tcp_connect(&c, US, OURP, THEM, THEIRP, ISS, now);
    pull(&c, now, &seg, NULL, NULL);
    feed(&c, PEER, ISS + 1, TCP_SYN | TCP_ACK, 4, NULL, 0, now);
    pull(&c, now, &seg, NULL, NULL);
    tcp_write(&c, "abcdefghij", 10);

    CHECK(pull(&c, now, &seg, data, &data_len), "something goes out");
    CHECK(seg.payload_len == 4,
          "but only as much as the other end said it would take");
    CHECK(!pull(&c, now, &seg, NULL, NULL),
          "and nothing more until the window opens");

    feed(&c, PEER + 1, ISS + 5, TCP_ACK, 6, NULL, 0, now);
    CHECK(pull(&c, now, &seg, data, &data_len), "an ack with room lets more go");
    CHECK(seg.payload_len == 6, "as much as the new window allows");

    /* a window of zero stops it entirely */
    establish(&c, now);
    tcp_write(&c, "abcdefghij", 10);
    feed(&c, PEER + 1, ISS + 1, TCP_ACK, 0, NULL, 0, now);
    /* drain the ack the data arrival may have queued */
    while (pull(&c, now, &seg, data, &data_len)) {
        CHECK(seg.payload_len == 0, "a zero window sends no data at all");
    }

    /*
     * the timer, which a real clock will never show you
     */

    now = 50000;
    establish(&c, now);
    tcp_write(&c, "again", 5);
    CHECK(pull(&c, now, &seg, NULL, NULL), "it goes out once");
    CHECK(!pull(&c, now + TCP_RTO_MS - 1, &seg, NULL, NULL),
          "and is not repeated before the timer expires");

    CHECK(pull(&c, now + TCP_RTO_MS, &seg, data, &data_len),
          "and is repeated when it does");
    CHECK(seg.seq == ISS + 1,
          "at the *same* sequence number, a retransmission is the same "
          "bytes in the same place, which is what lets the other end "
          "put a stream back together without caring how it was cut up");
    CHECK(seg.payload_len == 5 && memcmp(data, "again", 5) == 0,
          "and the same bytes");

    /* and it gives up eventually rather than trying forever */
    now = 100000;
    establish(&c, now);
    tcp_write(&c, "gone", 4);
    unsigned sends = 0;
    for (uint64_t t = now; t <= now + TCP_RTO_MS * (TCP_MAX_RETRIES + 4); t += 50) {
        if (pull(&c, t, &seg, NULL, NULL)) { sends++; }
    }
    CHECK(c.state == TCP_CLOSED,
          "a peer that stops acknowledging ends the connection");
    CHECK(c.error != NULL, "with a reason");
    CHECK(sends <= TCP_MAX_RETRIES + 1, "after a bounded number of tries");

    /*
     * seven of the eleven states are about stopping, and this is why
     */

    now = 200000;
    establish(&c, now);
    tcp_close(&c);
    CHECK(pull(&c, now, &seg, NULL, NULL), "close says fin");
    CHECK((seg.flags & TCP_FIN) != 0, "which is a fin");
    CHECK(c.state == TCP_FIN_WAIT_1, "and waits to be acknowledged");

    feed(&c, PEER + 1, ISS + 2, TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_FIN_WAIT_2,
          "acknowledged, and now waiting for the other end to finish, "
          "it may keep sending for as long as it likes");

    feed(&c, PEER + 1, ISS + 2, TCP_FIN | TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_TIME_WAIT, "their fin ends it");
    CHECK(pull(&c, now, &seg, NULL, NULL), "and is acknowledged");
    CHECK(seg.ack == PEER + 2, "counting their fin, which took a number");

    CHECK(!tcp_finished(&c, now), "but the connection is not gone yet");
    CHECK(!tcp_finished(&c, now + TCP_TIME_WAIT_MS - 1), "not a moment early");
    CHECK(tcp_finished(&c, now + TCP_TIME_WAIT_MS),
          "and gone when the wait is up, this is the one that leaves "
          "entries nobody can clear when it is missing");

    /* and the state itself moves, not merely the report of it. */
    CHECK(c.state == TCP_TIME_WAIT, "still waiting, until it is ticked");
    tcp_tick(&c, now + TCP_TIME_WAIT_MS, (uint8_t *)data, sizeof data);
    CHECK(c.state == TCP_CLOSED, "and a tick past the wait really closes it");

    /* while it waits, it still answers, which is the entire reason the state exists. */
    now = 205000;
    establish(&c, now);
    tcp_close(&c);
    pull(&c, now, &seg, NULL, NULL);
    feed(&c, PEER + 1, ISS + 2, TCP_FIN | TCP_ACK, 4096, NULL, 0, now);
    while (pull(&c, now, &seg, NULL, NULL)) { }
    CHECK(c.state == TCP_TIME_WAIT, "in TIME_WAIT with everything answered");

    feed(&c, PEER + 1, ISS + 2, TCP_FIN | TCP_ACK, 4096, NULL, 0, now + 100);
    CHECK(pull(&c, now + 100, &seg, NULL, NULL),
          "a repeated fin is answered again rather than ignored");
    CHECK((seg.flags & TCP_ACK) != 0 && seg.ack == PEER + 2,
          "with the same acknowledgement, without this the other end "
          "repeats its fin until it gives up, and TIME_WAIT is ten "
          "seconds of cost with none of the point");

    /* the fin waits for the data before it */
    now = 210000;
    establish(&c, now);
    tcp_write(&c, "last words", 10);
    tcp_close(&c);
    CHECK(pull(&c, now, &seg, data, &data_len), "the data goes first");
    CHECK(seg.payload_len == 10, "all of it");
    CHECK((seg.flags & TCP_FIN) == 0,
          "and the fin does not overtake it, a stream that ended before "
          "its last bytes would have them discarded");
    feed(&c, PEER + 1, ISS + 11, TCP_ACK, 4096, NULL, 0, now);
    CHECK(pull(&c, now, &seg, NULL, NULL), "then the fin");
    CHECK((seg.flags & TCP_FIN) != 0, "which is a fin");
    CHECK(seg.seq == ISS + 11, "at the number after the data");

    /*
     * and it waits for data that has not gone out *at all*, which is a
     * different thing from data that has gone and not been acknowledged.
     * with the peer's window shut, nothing can be sent, and a fin that
     * went anyway would end the stream before bytes the other end has
     * never seen
     */
    now = 215000;
    tcp_connect(&c, US, OURP, THEM, THEIRP, ISS, now);
    pull(&c, now, &seg, NULL, NULL);
    feed(&c, PEER, ISS + 1, TCP_SYN | TCP_ACK, 0, NULL, 0, now);
    while (pull(&c, now, &seg, NULL, NULL)) { }
    CHECK(tcp_write(&c, "unsendable", 10) == 10, "ten bytes are written");
    tcp_close(&c);
    while (pull(&c, now, &seg, NULL, NULL)) {
        CHECK((seg.flags & TCP_FIN) == 0,
              "no fin goes out while the window holds the data back");
    }
    CHECK(c.state != TCP_FIN_WAIT_1,
          "and the connection has not started closing either");

    feed(&c, PEER + 1, ISS + 1, TCP_ACK, 4096, NULL, 0, now);
    bool saw_data = false, saw_fin = false;
    while (pull(&c, now, &seg, data, &data_len)) {
        if (seg.payload_len > 0) { saw_data = true; }
        if (seg.flags & TCP_FIN) {
            CHECK(saw_data, "the data goes before the fin, once it can");
            saw_fin = true;
        }
    }
    CHECK(saw_data && saw_fin, "and both go once the window opens");



    now = 220000;
    establish(&c, now);
    feed(&c, PEER + 1, ISS + 1, TCP_FIN | TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_CLOSE_WAIT,
          "their fin does not end this end's half of the conversation");
    CHECK(pull(&c, now, &seg, NULL, NULL), "it is acknowledged");
    CHECK(seg.ack == PEER + 2, "counting their fin");

    CHECK(tcp_write(&c, "still here", 10) == 10,
          "and this end may keep sending, which many protocols rely on");
    CHECK(pull(&c, now, &seg, data, &data_len), "and it goes out");
    CHECK(seg.payload_len == 10, "all of it");
    feed(&c, PEER + 2, ISS + 11, TCP_ACK, 4096, NULL, 0, now);

    tcp_close(&c);
    CHECK(pull(&c, now, &seg, NULL, NULL), "and then this end finishes");
    CHECK((seg.flags & TCP_FIN) != 0, "with a fin");
    CHECK(c.state == TCP_LAST_ACK, "waiting to be acknowledged");

    feed(&c, PEER + 2, ISS + 12, TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_CLOSED,
          "and then it is closed, with no TIME_WAIT, because this end "
          "closed second and the other end is holding that");
    CHECK(tcp_finished(&c, now), "and can be forgotten at once");

    /* the bug four `fetch` commands found. */
    now = 225000;
    establish(&c, now);

    /* fill it, in mss-sized pieces the way a real sender would */
    {
        char lump[1400];
        memset(lump, 'x', sizeof lump);
        uint32_t seq = PEER + 1;
        for (int i = 0; i < 4; i++) {
            feed(&c, seq, ISS + 1, TCP_ACK, 4096, lump, sizeof lump, now);
            seq += sizeof lump;
        }
    }
    CHECK(tcp_readable(&c) == TCP_RECV_BUF, "the receive buffer is full");
    while (pull(&c, now, &seg, data, &data_len)) {
        CHECK(seg.window == 0,
              "and a full buffer advertises a window of zero, which is "
              "what stops the other end sending anything more, "
              "including its fin");
    }

    /* now the program gives up and closes without reading any of it */
    tcp_close(&c);
    CHECK(pull(&c, now, &seg, NULL, NULL), "closing says something");
    CHECK((seg.flags & TCP_RST) != 0,
          "and it is a *reset*, not a fin, those bytes were "
          "acknowledged and then thrown away, and a fin would claim the "
          "conversation ended properly when part of it was discarded");
    CHECK(c.state == TCP_CLOSED, "the connection ends at once");
    CHECK(tcp_finished(&c, now),
          "and its slot is free immediately, rather than being held by "
          "a wait for a fin the other end cannot send");

    /*
     * and a connection closed after being read properly still ends the
     * polite way
     */
    now = 226000;
    establish(&c, now);
    feed(&c, PEER + 1, ISS + 1, TCP_ACK, 4096, "abc", 3, now);
    tcp_read(&c, got, sizeof got);
    tcp_close(&c);
    CHECK(pull(&c, now, &seg, NULL, NULL), "a drained connection closes too");
    CHECK((seg.flags & TCP_FIN) != 0 && (seg.flags & TCP_RST) == 0,
          "with a fin, because nothing was thrown away");


    now = 227000;
    establish(&c, now);
    tcp_close(&c);
    while (pull(&c, now, &seg, NULL, NULL)) { }

    feed(&c, PEER + 1, ISS + 2, TCP_ACK, 4096, "ignored", 7, now);
    CHECK(tcp_readable(&c) == 0,
          "data arriving after the close is not buffered, nobody will "
          "ever read it");
    CHECK(pull(&c, now, &seg, NULL, NULL), "but it is acknowledged");
    CHECK(seg.ack == PEER + 8,
          "as *received*, so the other end may carry on and send its fin");
    CHECK(seg.window > 0,
          "and the window stays open, which is the whole point, a "
          "window that shut here is a connection that can never finish");



    now = 230000;
    establish(&c, now);
    tcp_close(&c);
    CHECK(pull(&c, now, &seg, NULL, NULL), "this end says fin");
    CHECK(c.state == TCP_FIN_WAIT_1, "and waits");

    /* their fin crosses ours, acknowledging only the data before it */
    feed(&c, PEER + 1, ISS + 1, TCP_FIN | TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_CLOSING,
          "two fins that crossed leave both ends waiting on the other");

    feed(&c, PEER + 2, ISS + 2, TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_TIME_WAIT, "and then the wait, as with any close");



    now = 240000;
    establish(&c, now);
    tcp_write(&c, "never sent", 10);
    feed(&c, PEER + 1, ISS + 1, TCP_RST, 4096, NULL, 0, now);
    CHECK(c.state == TCP_CLOSED, "a reset ends it immediately");
    CHECK(c.error != NULL, "with a reason to give");
    CHECK(!pull(&c, now, &seg, NULL, NULL),
          "and nothing further is sent, not even an acknowledgement, "
          "there is nothing left to acknowledge to");

    /*
     * a reset while connecting is the ordinary "nothing is listening
     * there", and is worth telling apart from a connection that was
     * established and then torn down, one is somebody else's correct
     * answer and the other is a fault
     */
    now = 245000;
    tcp_connect(&c, US, OURP, THEM, THEIRP, ISS, now);
    pull(&c, now, &seg, NULL, NULL);
    feed(&c, 0, ISS + 1, TCP_RST | TCP_ACK, 0, NULL, 0, now);
    CHECK(c.state == TCP_CLOSED, "a reset while connecting ends it");
    CHECK(c.error != NULL && strstr(c.error, "listening") != NULL,
          "and says nothing was listening, rather than the generic reason");

    establish(&c, now);
    feed(&c, PEER + 1, ISS + 1, TCP_RST, 4096, NULL, 0, now);
    CHECK(c.error != NULL && strstr(c.error, "listening") == NULL,
          "while a reset after it was established is a different thing "
          "and says so");

    /*
     * right for the first four billion bytes, which is the whole problem
     */

    CHECK(seq_lt(0xfffffff0u, 0x00000010u),
          "0xfffffff0 comes before 0x10, a plain < says the opposite");
    CHECK(seq_gt(0x00000010u, 0xfffffff0u), "and the other way round");
    CHECK(seq_lt(100, 200) && seq_gt(200, 100), "and ordinary numbers work");
    CHECK(seq_le(500, 500) && seq_ge(500, 500), "and equal is equal");

    /* the other two, across the wrap as well. */
    CHECK(!seq_ge(0xfffffff0u, 0x00000010u),
          "0xfffffff0 is not at or after 0x10");
    CHECK(seq_ge(0x00000010u, 0xfffffff0u), "and 0x10 is");
    CHECK(!seq_le(0x00000010u, 0xfffffff0u), "and the same for seq_le");
    CHECK(seq_le(0xfffffff0u, 0x00000010u), "both ways round");

    /* a whole connection across the boundary */
    now = 250000;
    uint32_t near_end = 0xffffffffu - 3;
    tcp_connect(&c, US, OURP, THEM, THEIRP, near_end, now);
    pull(&c, now, &seg, NULL, NULL);
    CHECK(seg.seq == near_end, "a connection can start near the top");

    feed(&c, PEER, near_end + 1, TCP_SYN | TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_ESTABLISHED, "and establish across the wrap");
    pull(&c, now, &seg, NULL, NULL);

    tcp_write(&c, "over the edge", 13);
    CHECK(pull(&c, now, &seg, data, &data_len), "and send across it");
    CHECK(seg.seq == near_end + 1, "from just below the boundary");
    CHECK(seg.payload_len == 13, "with all the bytes");

    /* the acknowledgement is on the other side of zero */
    feed(&c, PEER + 1, near_end + 14, TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.send_len == 0,
          "an acknowledgement that wrapped past zero is still an "
          "acknowledgement");

    /*
     * a stream is cut into segments at the mss, and a stack that sends
     * whatever it has in one go builds a packet the wire cannot carry,
     * which is then either fragmented at the ip layer or dropped
     */
    now = 260000;
    establish(&c, now);
    for (int i = 0; i < 30; i++) {
        tcp_write(&c, "0123456789012345678901234567890123456789"
                      "0123456789012345678901234567890123456789"
                      "0123456789012345678901234567890123456789"
                      "0123456789012345678901234567890123456789"
                      "0123456789012345678901234567890123456789", 200);
    }
    /*
     * the peer's window is 4096, so the first segment is limited by the
     * mss rather than by it
     */
    CHECK(pull(&c, now, &seg, data, &data_len), "a lot of data goes out");
    CHECK(seg.payload_len == TCP_MSS,
          "one segment at a time, and no larger than the mss, a bigger "
          "one is a frame the wire will not carry");



    uint8_t raw[600];
    size_t n = peer_segment(raw, PEER, ISS, TCP_ACK, 4096, "body", 4);
    CHECK(tcp_parse(raw, n, THEM, US, &seg), "a well-formed segment parses");
    CHECK(seg.seq == PEER && seg.ack == ISS, "with its numbers");
    CHECK(seg.payload_len == 4, "and its payload");

    raw[16] ^= 0xff;
    CHECK(!tcp_parse(raw, n, THEM, US, &seg), "a bad checksum is refused");
    raw[16] ^= 0xff;

    CHECK(!tcp_parse(raw, n, US, US, &seg),
          "and so is the right checksum with the wrong addresses, which "
          "is what the pseudo-header is for");

    CHECK(!tcp_parse(raw, 19, THEM, US, &seg),
          "a segment shorter than a header is refused");

    /* a data offset pointing past the end of the segment. */
    n = peer_segment(raw, PEER, ISS, TCP_ACK, 4096, "body", 4);
    raw[12] = 15 << 4;          /* 60 bytes of header in a 24-byte segment */

    /* and then the checksum is recomputed over the altered header. */
    {
        raw[16] = raw[17] = 0;
        uint8_t ph[12] = { 10, 0, 2, 2, 10, 0, 2, 15, 0, 6,
                           (uint8_t)(n >> 8), (uint8_t)n };
        uint32_t acc = 0;
        for (int i = 0; i < 12; i += 2) {
            acc += ((uint32_t)ph[i] << 8) | ph[i + 1];
        }
        uint16_t ck = sum16(raw, n, acc);
        raw[16] = (uint8_t)(ck >> 8); raw[17] = (uint8_t)ck;
    }
    CHECK(!tcp_parse(raw, n, THEM, US, &seg),
          "a header longer than the segment claiming to hold it is refused, "
          "even when its checksum is perfectly correct");

    /* and one that is exactly the segment, with no payload */
    n = peer_segment(raw, PEER, ISS, TCP_ACK, 4096, NULL, 0);
    CHECK(tcp_parse(raw, n, THEM, US, &seg) && seg.payload_len == 0,
          "a bare header is a legal segment with nothing in it");



    memset(&c, 0, sizeof c);
    CHECK(!pull(&c, now, &seg, NULL, NULL), "a closed connection says nothing");
    feed(&c, PEER, ISS, TCP_SYN | TCP_ACK, 4096, NULL, 0, now);
    CHECK(c.state == TCP_CLOSED, "and takes nothing it is sent");
    CHECK(tcp_write(&c, "hello", 5) == 0, "and accepts nothing to write");

    if (failures == 0) printf("all good\n");
    return failures;
}

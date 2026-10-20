// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/tcp.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * tcp: the first thing here that has to remember what it said.
 */

/* the design notes for tcp.h are in docs/subsystems/net.rst */

#ifndef VELVET_TCP_H
#define VELVET_TCP_H

#include <velvet/net.h>

/* tcp: the first thing here that has to remember what it said. */

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_HEADER_MIN 20

#define TCP_MSS 1460

#define TCP_SEND_BUF 4096
#define TCP_RECV_BUF 4096

#define TCP_CONN_MAX 4

struct tcp_segment {
    uint16_t from_port, to_port;
    uint32_t seq, ack;
    uint8_t  flags;
    uint16_t window;
    size_t   payload_at, payload_len;
};

bool tcp_parse(const uint8_t *p, size_t len, ipv4 from, ipv4 to,
               struct tcp_segment *out);

size_t tcp_build(uint8_t *p, ipv4 from, ipv4 to,
                 uint16_t from_port, uint16_t to_port,
                 uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                 const void *payload, size_t payload_len);

static inline bool seq_lt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}
static inline bool seq_le(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) <= 0;
}
static inline bool seq_gt(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}
static inline bool seq_ge(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,     /* the kernel said fin, waiting for it to be acknowledged */
    TCP_FIN_WAIT_2,     /* it was; waiting for the other end's fin */
    TCP_CLOSING,        /* the project both said fin at once */
    TCP_TIME_WAIT,      /* done, but staying for the stragglers */
    TCP_CLOSE_WAIT,     /* the other end is finished; this one is not */
    TCP_LAST_ACK        /* and now it is, waiting to be acknowledged */
};

#define TCP_RTO_MS      500
#define TCP_MAX_RETRIES 6

#define TCP_TIME_WAIT_MS 10000

struct tcp_conn {
    enum tcp_state state;

    /*
     * the pid that opened it, so a program that dies does not leave a
     * connection nobody can reach and nothing will close. -1 for one the
     * kernel opened on its own account, which is what the shell does
     */
    int owner;

    ipv4     local, remote;
    uint16_t local_port, remote_port;

    uint32_t snd_una, snd_nxt;
    uint16_t snd_window;        /* what the other end says it will take */

    uint32_t rcv_nxt;

    uint8_t send_buf[TCP_SEND_BUF];
    size_t  send_len;           /* bytes held, starting at snd_una */

    uint8_t recv_buf[TCP_RECV_BUF];
    size_t  recv_len;

    uint64_t timer_ms;
    unsigned retries;
    bool     timer_running;

    bool ack_due;

    bool closing;

    bool done_reading;

    bool reset_due;
    bool fin_sent;
    uint32_t fin_seq;           /* the sequence number the fin occupied */

    uint64_t closed_ms;

    /* why it ended, for anything that has to explain itself. */
    const char *error;
};

void tcp_listen(struct tcp_conn *c, ipv4 local, uint16_t port, uint32_t iss);

void tcp_connect(struct tcp_conn *c, ipv4 local, uint16_t local_port,
                 ipv4 remote, uint16_t remote_port, uint32_t iss,
                 uint64_t now_ms);

size_t tcp_write(struct tcp_conn *c, const void *data, size_t len);
size_t tcp_read(struct tcp_conn *c, void *out, size_t max);

size_t tcp_writable(const struct tcp_conn *c);
size_t tcp_readable(const struct tcp_conn *c);

void tcp_close(struct tcp_conn *c);

void tcp_reset(struct tcp_conn *c, uint64_t now_ms);

/*
 * a state machine with no clock of its own: told the time and handed
 * segments, it *returns* what to send. that is what makes it testable,
 * since a retransmission timer, a two-minute wait and a
 * four-billion-byte sequence wrap are all things a real clock will never
 * show you
 */
void tcp_input(struct tcp_conn *c, const struct tcp_segment *seg,
               const uint8_t *payload, uint64_t now_ms);

size_t tcp_tick(struct tcp_conn *c, uint64_t now_ms, uint8_t *out,
                size_t max);

bool tcp_finished(const struct tcp_conn *c, uint64_t now_ms);

const char *tcp_state_name(enum tcp_state s);

#endif

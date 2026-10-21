// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/tcp.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * tcp: the first thing here that has to remember what it said.
 */

#include <velvet/tcp.h>
#include <velvet/ip.h>
#include <velvet/string.h>

/* the same pseudo-header udp uses, with tcp's protocol number and the whole segment's length. */
static void sum_pseudo(struct net_sum *s, ipv4 from, ipv4 to, uint16_t len)
{
    uint8_t p[12];
    put32be(p,     from);
    put32be(p + 4, to);
    p[8] = 0;
    p[9] = IP_PROTO_TCP;
    put16be(p + 10, len);
    net_sum_add(s, p, sizeof p);
}

bool tcp_parse(const uint8_t *p, size_t len, ipv4 from, ipv4 to,
               struct tcp_segment *out)
{
    if (len < TCP_HEADER_MIN || len > 0xffff) {
        return false;
    }

    /* the top four bits of byte 12 are the header length in 32-bit words. */
    size_t header = (size_t)(p[12] >> 4) * 4;
    if (header < TCP_HEADER_MIN || header > len) {
        return false;
    }

    struct net_sum s;
    net_sum_start(&s);
    sum_pseudo(&s, from, to, (uint16_t)len);
    net_sum_add(&s, p, len);
    if (net_sum_finish(&s) != 0) {
        return false;
    }

    out->from_port   = get16be(p);
    out->to_port     = get16be(p + 2);
    out->seq         = get32be(p + 4);
    out->ack         = get32be(p + 8);
    out->flags       = p[13];
    out->window      = get16be(p + 14);
    out->payload_at  = header;
    out->payload_len = len - header;
    return true;
}

size_t tcp_build(uint8_t *p, ipv4 from, ipv4 to,
                 uint16_t from_port, uint16_t to_port,
                 uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window,
                 const void *payload, size_t payload_len)
{
    memset(p, 0, TCP_HEADER_MIN);

    put16be(p,      from_port);
    put16be(p + 2,  to_port);
    put32be(p + 4,  seq);
    put32be(p + 8,  ack);
    p[12] = (TCP_HEADER_MIN / 4) << 4;   /* no options */
    p[13] = flags;
    put16be(p + 14, window);
    put16be(p + 16, 0);                  /* checksum, over a zero */
    put16be(p + 18, 0);                  /* urgent pointer, never used */

    if (payload_len > 0) {
        memcpy(p + TCP_HEADER_MIN, payload, payload_len);
    }
    size_t len = TCP_HEADER_MIN + payload_len;

    struct net_sum s;
    net_sum_start(&s);
    sum_pseudo(&s, from, to, (uint16_t)len);
    net_sum_add(&s, p, len);

    /*
     * and here, unlike udp, a computed zero stays zero, tcp has no
     * "not computed" value to collide with, so there is nothing to
     * fold it away from
     */
    put16be(p + 16, net_sum_finish(&s));
    return len;
}



static void reset_conn(struct tcp_conn *c)
{
    memset(c, 0, sizeof *c);
    c->snd_window = TCP_MSS;
}

void tcp_listen(struct tcp_conn *c, ipv4 local, uint16_t port,
                uint32_t iss)
{
    reset_conn(c);
    c->state      = TCP_LISTEN;
    c->local      = local;
    c->local_port = port;

    /*
     * a listening end needs an initial sequence number as much as a
     * connecting one does, and for the same reasons, it is the first
     * thing it will say. the first version of this took no `iss` and so
     * every incoming connection started at zero, which is both guessable
     * and repeated: two connections on the same two ports would use the
     * same numbers, and a straggler from the first is then perfectly
     * acceptable data in the second
     */
    c->snd_una = iss;
}

void tcp_connect(struct tcp_conn *c, ipv4 local, uint16_t local_port,
                 ipv4 remote, uint16_t remote_port, uint32_t iss,
                 uint64_t now_ms)
{
    reset_conn(c);
    c->state       = TCP_SYN_SENT;
    c->local       = local;
    c->local_port  = local_port;
    c->remote      = remote;
    c->remote_port = remote_port;

    /*
     * the syn occupies one sequence number, which is what makes it
     * acknowledgeable by the same arithmetic as data. so nxt is one past
     * the initial number before anything has been sent
     */
    c->snd_una = iss;
    c->snd_nxt = iss + 1;

    c->timer_ms      = now_ms;
    c->timer_running = true;
    c->retries       = 0;
}



size_t tcp_writable(const struct tcp_conn *c)
{
    return TCP_SEND_BUF - c->send_len;
}

size_t tcp_readable(const struct tcp_conn *c)
{
    return c->recv_len;
}

size_t tcp_write(struct tcp_conn *c, const void *data, size_t len)
{
    if (c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) {
        return 0;
    }
    if (c->closing) {
        /* fin has been asked for. */
        return 0;
    }
    size_t room = tcp_writable(c);
    if (len > room) {
        len = room;
    }
    memcpy(c->send_buf + c->send_len, data, len);
    c->send_len += len;
    return len;
}

size_t tcp_read(struct tcp_conn *c, void *out, size_t max)
{
    size_t n = c->recv_len < max ? c->recv_len : max;
    if (n == 0) {
        return 0;
    }
    memcpy(out, c->recv_buf, n);

    /*
     * TODO: draining the buffer opens the window and nothing tells the
     * other end. what it was last told is what it will keep respecting, so
     * after it has filled a receive buffer the transfer stops until its
     * own retransmit timer fires and the retransmission that lands here
     * sets ack_due again. sending an acknowledgment from this call once
     * what is free has grown by a segment is what unlocks it.
     */
    /* the rest slides down. */
    memmove(c->recv_buf, c->recv_buf + n, c->recv_len - n);
    c->recv_len -= n;
    return n;
}

void tcp_close(struct tcp_conn *c)
{
    if (c->state == TCP_LISTEN || c->state == TCP_SYN_SENT) {
        /* nothing was ever established, so there is nobody to say fin to. */
        c->state = TCP_CLOSED;
        return;
    }

    /* nobody is going to read this connection again */
    c->done_reading = true;

    /*
     * closing with data still unread is not a polite ending, and
     * pretending otherwise is worse than saying so. the bytes were
     * acknowledged, this end told the other they had arrived, and
     * now nothing will ever look at them. every stack sends a reset
     * here, and the reason is exactly that: a fin would claim the
     * conversation finished properly when part of it was thrown away
     */
    if (c->recv_len > 0 && c->state != TCP_TIME_WAIT
     && c->state != TCP_CLOSED) {
        c->recv_len  = 0;
        c->reset_due = true;
        return;
    }
    c->closing = true;
}

void tcp_reset(struct tcp_conn *c, uint64_t now_ms)
{
    c->state     = TCP_CLOSED;
    c->closed_ms = now_ms;
    c->error     = "the connection was reset";
}

/* how much this end will accept, which is whatever is free in the receive buffer. */
static uint16_t window_of(const struct tcp_conn *c)
{
    return (uint16_t)(TCP_RECV_BUF - c->recv_len);
}



/* everything up to `ack` has been confirmed, so it may be forgotten */
static void acknowledge(struct tcp_conn *c, uint32_t ack)
{
    if (!seq_gt(ack, c->snd_una)) {
        return;             /* nothing new. a duplicate ack */
    }
    if (seq_gt(ack, c->snd_nxt)) {
        return;             /* acknowledging what was never sent */
    }

    uint32_t freed = ack - c->snd_una;

    /*
     * the syn and the fin each occupy a sequence number but no byte of
     * the buffer, so they are subtracted out before the buffer is moved.
     * getting this wrong drops a byte of real data for every handshake,
     * which shows up as a stream that is right except for the first
     * character
     */
    if (c->fin_sent && seq_ge(ack, c->fin_seq + 1)) {
        freed -= 1;
    }

    if (freed > c->send_len) {
        freed = (uint32_t)c->send_len;
    }
    if (freed > 0) {
        memmove(c->send_buf, c->send_buf + freed, c->send_len - freed);
        c->send_len -= freed;
    }
    c->snd_una = ack;

    /* the timer covers the oldest unacknowledged thing. */
    c->retries = 0;
    if (c->snd_una == c->snd_nxt) {
        c->timer_running = false;
    }
}

static void take_payload(struct tcp_conn *c, const struct tcp_segment *seg,
                         const uint8_t *payload)
{
    if (seg->payload_len == 0) {
        return;
    }

    /* only what comes next. */
    if (seg->seq != c->rcv_nxt) {
        /*
         * it is still worth acknowledging, and with the number this end
         * actually wants: a duplicate ack is how the other end finds out
         * that something went missing
         */
        c->ack_due = true;
        return;
    }

    /* nobody will ever read this. */
    if (c->done_reading) {
        c->rcv_nxt += (uint32_t)seg->payload_len;
        c->ack_due  = true;
        return;
    }

    size_t room = TCP_RECV_BUF - c->recv_len;
    size_t n = seg->payload_len;
    if (n > room) {
        /* more than the advertised window. */
        n = room;
    }
    if (n == 0) {
        c->ack_due = true;
        return;
    }
    memcpy(c->recv_buf + c->recv_len, payload, n);
    c->recv_len += n;
    c->rcv_nxt  += (uint32_t)n;
    c->ack_due   = true;
}

void tcp_input(struct tcp_conn *c, const struct tcp_segment *seg,
               const uint8_t *payload, uint64_t now_ms)
{
    if (c->state == TCP_CLOSED) {
        return;
    }

    /* a reset ends it, in every state. */
    if (seg->flags & TCP_RST) {
        if (c->state != TCP_LISTEN) {
            /*
             * a reset while still connecting means one thing and only
             * one: there is a machine at that address and *nothing is
             * listening on that port*. saying so is worth a branch,
             * it is the single most common way a connection fails, and
             * the generic message sends you looking at your own stack
             * for what is a correct answer from somebody else's
             */
            c->error = (c->state == TCP_SYN_SENT)
                     ? "nothing is listening on that port"
                     : "the other end reset the connection";
            c->state     = TCP_CLOSED;
            c->closed_ms = now_ms;
        }
        return;
    }

    c->snd_window = seg->window;

    switch (c->state) {
    case TCP_LISTEN:
        if (!(seg->flags & TCP_SYN)) {
            return;
        }
        /* somebody is connecting. */
        c->remote_port = seg->from_port;
        c->rcv_nxt     = seg->seq + 1;  /* their syn takes one */

        /*
         * this end's syn takes one number, exactly as the other end's
         * did
         */
        c->snd_nxt = c->snd_una + 1;

        c->state         = TCP_SYN_RECEIVED;
        c->timer_ms      = now_ms;
        c->timer_running = true;
        c->retries       = 0;
        return;

    case TCP_SYN_SENT:
        if (!(seg->flags & TCP_SYN)) {
            return;
        }
        if (seg->flags & TCP_ACK) {
            /* the ordinary case: syn+ack. */
            if (seg->ack != c->snd_nxt) {
                return;
            }
            c->rcv_nxt       = seg->seq + 1;
            acknowledge(c, seg->ack);
            c->state         = TCP_ESTABLISHED;
            c->ack_due       = true;
            c->timer_running = false;
        } else {
            /* both ends called connect at once. */
            c->rcv_nxt = seg->seq + 1;
            c->state   = TCP_SYN_RECEIVED;
            c->ack_due = true;
        }
        return;

    case TCP_SYN_RECEIVED:
        if (!(seg->flags & TCP_ACK) || seg->ack != c->snd_nxt) {
            return;
        }
        acknowledge(c, seg->ack);
        c->state         = TCP_ESTABLISHED;
        c->timer_running = false;
        /* fall through: this segment may carry data as well */
        break;

    default:
        break;
    }

    if (seg->flags & TCP_ACK) {
        acknowledge(c, seg->ack);
    }

    /* data, in the states that can take it */
    if (c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1
     || c->state == TCP_FIN_WAIT_2) {
        take_payload(c, seg, payload);
    }

    /* anything already seen is acknowledged again. */
    if (seq_lt(seg->seq, c->rcv_nxt)
     && (seg->payload_len > 0 || (seg->flags & TCP_FIN))) {
        c->ack_due = true;
    }

    /*
     * only in sequence. a fin that arrives ahead of data still missing
     * would end the stream early and lose whatever was in the gap, and
     * out-of-order data is dropped here, so that gap is a real
     * possibility rather than a theoretical one
     */
    if ((seg->flags & TCP_FIN) && seg->seq + seg->payload_len == c->rcv_nxt) {
        c->rcv_nxt += 1;            /* the fin occupies one */
        c->ack_due  = true;

        switch (c->state) {
        case TCP_ESTABLISHED:
            /* the other end is finished. */
            c->state = TCP_CLOSE_WAIT;
            break;

        case TCP_FIN_WAIT_1:
            /*
             * both said fin. whether this ends in CLOSING or TIME_WAIT
             * depends on whether ours was acknowledged in the same
             * segment
             */
            if (seq_ge(c->snd_una, c->fin_seq + 1)) {
                c->state     = TCP_TIME_WAIT;
                c->closed_ms = now_ms;
            } else {
                c->state = TCP_CLOSING;
            }
            break;

        case TCP_FIN_WAIT_2:
            c->state     = TCP_TIME_WAIT;
            c->closed_ms = now_ms;
            break;

        default:
            break;
        }
        return;
    }

    /* an acknowledgement of this end's fin, with no fin of their own */
    if ((seg->flags & TCP_ACK) && c->fin_sent
     && seq_ge(c->snd_una, c->fin_seq + 1)) {
        switch (c->state) {
        case TCP_FIN_WAIT_1:
            c->state = TCP_FIN_WAIT_2;
            break;
        case TCP_CLOSING:
            c->state     = TCP_TIME_WAIT;
            c->closed_ms = now_ms;
            break;
        case TCP_LAST_ACK:
            /* both ends said fin and both were acknowledged. */
            c->state     = TCP_CLOSED;
            c->closed_ms = now_ms;
            break;
        default:
            break;
        }
    }
}



/*
 * what may go out right now: what is in the buffer and not yet sent,
 * limited by what the other end says it will take
 */
static size_t sendable(const struct tcp_conn *c)
{
    uint32_t in_flight = c->snd_nxt - c->snd_una;

    /*
     * the fin occupies a sequence number but no buffer byte, so it does
     * not count against what is left to send
     */
    if (c->fin_sent) {
        in_flight -= 1;
    }
    if (in_flight >= c->send_len) {
        return 0;
    }
    size_t waiting = c->send_len - in_flight;

    size_t window = c->snd_window > in_flight ? c->snd_window - in_flight : 0;
    if (waiting > window) {
        waiting = window;
    }
    if (waiting > TCP_MSS) {
        waiting = TCP_MSS;
    }
    return waiting;
}

size_t tcp_tick(struct tcp_conn *c, uint64_t now_ms, uint8_t *out,
                size_t max)
{
    if (max < TCP_HEADER_MIN || c->state == TCP_CLOSED) {
        return 0;
    }

    /* TIME_WAIT is a connection doing nothing on purpose. */
    if (c->state == TCP_TIME_WAIT) {
        if (now_ms - c->closed_ms >= TCP_TIME_WAIT_MS) {
            c->state = TCP_CLOSED;
            return 0;
        }
        /*
         * it still answers. that is the entire reason this state exists
         *, the other end's fin needs acknowledging, and if that
         * acknowledgement is lost it will send the fin again and needs
         * somebody still here to answer it.
         *
         * the first version of this returned zero for the whole state,
         * which made TIME_WAIT a connection sitting silently doing
         * nothing for ten seconds: all of the cost and none of the
         * point. the other end would have retransmitted its fin until
         * it gave up
         */
        if (c->ack_due) {
            c->ack_due = false;
            return tcp_build(out, c->local, c->remote, c->local_port,
                             c->remote_port, c->snd_nxt, c->rcv_nxt,
                             TCP_ACK, window_of(c), NULL, 0);
        }
        return 0;
    }


    if (c->reset_due) {
        c->reset_due = false;
        c->state     = TCP_CLOSED;
        c->closed_ms = now_ms;
        c->error     = "closed with data still unread";
        return tcp_build(out, c->local, c->remote, c->local_port,
                         c->remote_port, c->snd_nxt, c->rcv_nxt,
                         TCP_RST | TCP_ACK, 0, NULL, 0);
    }


    if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED) {
        if (c->timer_running && now_ms - c->timer_ms < TCP_RTO_MS
            && c->retries > 0) {
            return 0;
        }
        if (c->retries >= TCP_MAX_RETRIES) {
            c->state     = TCP_CLOSED;
            c->closed_ms = now_ms;
            c->error     = "nobody answered";
            return 0;
        }
        c->timer_ms      = now_ms;
        c->timer_running = true;
        c->retries++;
        c->ack_due = false;

        uint8_t flags = TCP_SYN
                      | (c->state == TCP_SYN_RECEIVED ? TCP_ACK : 0);
        return tcp_build(out, c->local, c->remote, c->local_port,
                         c->remote_port, c->snd_una, c->rcv_nxt, flags,
                         window_of(c), NULL, 0);
    }

    /* one timer for the whole connection, covering whatever is oldest and unacknowledged. */
    if (c->timer_running && now_ms - c->timer_ms >= TCP_RTO_MS) {
        if (c->retries >= TCP_MAX_RETRIES) {
            c->state     = TCP_CLOSED;
            c->closed_ms = now_ms;
            c->error     = "the other end stopped acknowledging";
            return 0;
        }
        c->retries++;
        c->timer_ms = now_ms;

        /* back to the oldest unacknowledged byte. */
        c->snd_nxt = c->snd_una;
        if (c->fin_sent) {
            c->fin_sent = false;
        }
    }


    size_t n = sendable(c);
    if (n > 0) {
        size_t at = c->snd_nxt - c->snd_una;
        if (TCP_HEADER_MIN + n > max) {
            n = max - TCP_HEADER_MIN;
        }
        size_t len = tcp_build(out, c->local, c->remote, c->local_port,
                               c->remote_port, c->snd_nxt, c->rcv_nxt,
                               TCP_ACK | TCP_PSH, window_of(c),
                               c->send_buf + at, n);
        c->snd_nxt += (uint32_t)n;
        c->ack_due  = false;

        if (!c->timer_running) {
            c->timer_running = true;
            c->timer_ms      = now_ms;
            c->retries       = 0;
        }
        return len;
    }

    /* only once everything written has been sent. */
    if (c->closing && !c->fin_sent && c->snd_nxt - c->snd_una >= c->send_len) {
        c->fin_seq  = c->snd_nxt;
        c->snd_nxt += 1;
        c->fin_sent = true;
        c->ack_due  = false;

        switch (c->state) {
        case TCP_ESTABLISHED:
            c->state = TCP_FIN_WAIT_1;
            break;
        case TCP_CLOSE_WAIT:
            c->state = TCP_LAST_ACK;
            break;
        default:
            break;
        }

        if (!c->timer_running) {
            c->timer_running = true;
            c->timer_ms      = now_ms;
            c->retries       = 0;
        }
        return tcp_build(out, c->local, c->remote, c->local_port,
                         c->remote_port, c->fin_seq, c->rcv_nxt,
                         TCP_FIN | TCP_ACK, window_of(c), NULL, 0);
    }

    /* last, because anything above would have carried it. */
    if (c->ack_due) {
        c->ack_due = false;
        return tcp_build(out, c->local, c->remote, c->local_port,
                         c->remote_port, c->snd_nxt, c->rcv_nxt, TCP_ACK,
                         window_of(c), NULL, 0);
    }
    return 0;
}

bool tcp_finished(const struct tcp_conn *c, uint64_t now_ms)
{
    if (c->state == TCP_CLOSED) {
        return true;
    }
    if (c->state == TCP_TIME_WAIT) {
        return now_ms - c->closed_ms >= TCP_TIME_WAIT_MS;
    }
    return false;
}


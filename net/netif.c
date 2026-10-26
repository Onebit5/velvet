// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/netif.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the interface: the one place that knows both card and stack.
 */

#include <velvet/netif.h>
#include <velvet/ether.h>
#include <velvet/ip.h>
#include "icmp.h"
#include "udp.h"
#include <velvet/socket.h>
#include <velvet/tcp.h>
#include <velvet/e1000.h>
#include <velvet/pit.h>
#include <velvet/string.h>
#include <velvet/kprintf.h>
#include <velvet/spinlock.h>
#include <velvet/sched.h>
#include <velvet/thread.h>

/* NOTE: everything this file *calls* is checked against packets built from the rfcs. */

static bool up;
static ipv4 my_ip, my_mask;
static ipv4 my_router;

static struct arp_cache cache;

/* the interface's state, which the poll thread writes and the calls read. */
static struct spinlock net_lock = SPINLOCK("net", LOCK_RANK_DEVICE);

/* one buffer for building, one for receiving. */
static uint8_t out_frame[ETHER_HEADER_LEN + ETHER_MTU];
static uint8_t in_frame[2048];

bool net_is_up(void)
{
    return up;
}
ipv4 net_address(void)
{
    return my_ip;
}
ipv4 net_netmask(void)
{
    return my_mask;
}
const struct mac *net_mac(void)
{
    return e1000_mac();
}
bool net_up(ipv4 address, ipv4 netmask, ipv4 router)
{
    if (!e1000_present()) {
        return false;
    }
    uint64_t flags = spin_lock_irq(&net_lock);
    my_ip = address;
    my_mask = netmask;
    my_router = router;
    arp_cache_reset(&cache);
    up = true;
    spin_unlock_irq(&net_lock, flags);
    return true;
}

/* the receive path, needed by the send path, see the loopback note in net_send_ip. */
static void got_ip(const uint8_t *p, size_t len);


static bool send_frame(const struct mac *to, uint16_t type,
                       const void *payload, size_t len)
{
    /*
     * FIXME: this buffer is one for the whole machine, and so are the two
     * static datagrams the callers build into (net_send_ip and
     * net_socket_send each declare one). two processes sending at once on
     * two cores assemble into the same bytes: a frame can leave carrying
     * one program's header and another program's payload, and a short
     * datagram written over a long one leaves the long one's tail in
     * place. nothing on this path takes a lock, which is deliberate in
     * net_socket_send, the socket table is locked and the buffers it
     * feeds are not. one transmit lock across build and send, or a
     * buffer per core, makes a frame one unit again.
     */
    if (len > ETHER_MTU) {
        return false;
    }
    size_t at = ether_build(out_frame, to, e1000_mac(), type);
    memcpy(out_frame + at, payload, len);

    if (!e1000_send(out_frame, at + len)) {
        return false;
    }
    return true;
}

bool net_arp_request(ipv4 who)
{
    if (!up) {
        return false;
    }
    uint8_t p[ARP_PACKET_LEN];
    arp_build(p, ARP_REQUEST, e1000_mac(), my_ip, &MAC_ZERO, who);

    return send_frame(&MAC_BROADCAST, ETHERTYPE_ARP, p, sizeof p);
}

/*
 * a table of connections rather than a list, for the same reason the
 * socket table is one: this runs on a poll thread and is read by the
 * calls a program makes, and a fixed table with a lock over it has no
 * allocation to get wrong at either end.
 *
 * four of them, and that is not a small number here, each carries
 * eight kilobytes of buffers, so the table is a third of a megabyte of
 * static memory. that is the honest cost of a protocol that must keep
 * what it has said until the other end confirms it
 */
static struct tcp_conn conns[TCP_CONN_MAX];
static uint16_t next_port = 40000;

/* one buffer for building segments, on the poll thread */
static uint8_t tcp_out[TCP_HEADER_MIN + TCP_MSS];

static struct tcp_conn *find_conn(ipv4 from, uint16_t from_port,
                                  uint16_t to_port)
{
    struct tcp_conn *listening = NULL;

    for (int i = 0; i < TCP_CONN_MAX; i++) {
        struct tcp_conn *c = &conns[i];
        if (c->state == TCP_CLOSED) {
            continue;
        }
        if (c->local_port != to_port) {
            continue;
        }
        if (c->state == TCP_LISTEN) {
            listening = c;
            continue;
        }
        if (c->remote == from && c->remote_port == from_port) {
            return c;
        }
    }
    /*
     * an established connection is preferred over a listening one, which
     * is why the whole table is walked before this is returned: a
     * segment belonging to a live connection must never be handed to
     * the listener on the same port
     */
    return listening;
}

static void got_tcp(ipv4 from, ipv4 to, const uint8_t *p, size_t len)
{
    struct tcp_segment seg;
    if (!tcp_parse(p, len, from, to, &seg)) {
        return;
    }

    struct tcp_conn *c = find_conn(from, seg.from_port, seg.to_port);
    if (c == NULL) {
        /*
         * nobody is listening. the right answer is a reset, so the other
         * end finds out now rather than retrying for a minute, and it
         * is built by hand because there is no connection to build it
         * from
         */
        if (!(seg.flags & TCP_RST)) {
            size_t n = tcp_build(tcp_out, to, from, seg.to_port,
                                 seg.from_port, seg.ack,
                                 seg.seq + seg.payload_len
                                     + ((seg.flags & TCP_SYN) ? 1 : 0),
                                 TCP_RST | TCP_ACK, 0, NULL, 0);
            net_send_ip(from, IP_PROTO_TCP, tcp_out, n);
        }
        return;
    }

    if (c->state == TCP_LISTEN) {
        /*
         * the address is in the ip header and a segment does not carry
         * it, so it is filled in here before the state machine sees it
         */
        c->remote = from;
    }
    tcp_input(c, &seg, p + seg.payload_at, pit_uptime_ms());
}

/* give every connection its chance to speak. */
static void tcp_run(void)
{
    uint64_t now = pit_uptime_ms();

    for (int i = 0; i < TCP_CONN_MAX; i++) {
        struct tcp_conn *c = &conns[i];
        if (c->state == TCP_CLOSED) {
            continue;
        }
        for (int guard = 0; guard < 16; guard++) {
            size_t n = tcp_tick(c, now, tcp_out, sizeof tcp_out);
            if (n == 0) {
                break;
            }
            if (!net_send_ip(c->remote, IP_PROTO_TCP, tcp_out, n)) {
                /* arp has not answered yet. */
                break;
            }
        }
    }
}

int net_tcp_connect(ipv4 to, uint16_t port)
{
    if (!up) {
        return -1;
    }

    /* a finished connection still holding its slot is fair game. */
    uint64_t now = pit_uptime_ms();
    bool any_free = false;
    for (int i = 0; i < TCP_CONN_MAX; i++) {
        if (tcp_finished(&conns[i], now)) {
            any_free = true;
            break;
        }
    }
    if (!any_free) {
        int oldest = -1;
        for (int i = 0; i < TCP_CONN_MAX; i++) {
            if (conns[i].state != TCP_TIME_WAIT) {
                continue;
            }
            if (oldest < 0 || conns[i].closed_ms < conns[oldest].closed_ms) {
                oldest = i;
            }
        }
        if (oldest >= 0) {
            memset(&conns[oldest], 0, sizeof conns[oldest]);
        }
    }

    for (int i = 0; i < TCP_CONN_MAX; i++) {
        if (!tcp_finished(&conns[i], now)) {
            continue;
        }
        /* the initial sequence number has to differ between connections. */
        uint32_t iss = (uint32_t)(pit_uptime_ms() * 2654435761u) + next_port;

        tcp_connect(&conns[i], my_ip, next_port, to, port, iss,
                    pit_uptime_ms());
        next_port++;
        if (next_port < 40000) {
            next_port = 40000;
        }
        return i;
    }
    return -1;
}

int net_tcp_listen(uint16_t port)
{
    if (!up) {
        return -1;
    }
    for (int i = 0; i < TCP_CONN_MAX; i++) {
        if (conns[i].state == TCP_CLOSED) {
            uint32_t iss = (uint32_t)(pit_uptime_ms() * 2654435761u) + port;
            tcp_listen(&conns[i], my_ip, port, iss);
            return i;
        }
    }
    return -1;
}

static struct tcp_conn *owned(int owner, int handle)
{
    int i = handle - TCP_HANDLE_BASE;
    if (i < 0 || i >= TCP_CONN_MAX) {
        return NULL;
    }
    struct tcp_conn *c = &conns[i];
    if (c->state == TCP_CLOSED && c->error == NULL) {
        return NULL;
    }
    return c->owner == owner ? c : NULL;
}

int net_tcp_open(int owner, ipv4 to, uint16_t port)
{
    int i = net_tcp_connect(to, port);
    if (i < 0) {
        return -1;
    }
    conns[i].owner = owner;
    return TCP_HANDLE_BASE + i;
}

int net_tcp_serve(int owner, uint16_t port)
{
    int i = net_tcp_listen(port);
    if (i < 0) {
        return -1;
    }
    conns[i].owner = owner;
    return TCP_HANDLE_BASE + i;
}

int64_t net_tcp_send(int owner, int handle, const void *data, size_t len)
{
    struct tcp_conn *c = owned(owner, handle);
    if (c == NULL) {
        return -1;
    }
    uint64_t flags = spin_lock_irq(&net_lock);
    size_t n = tcp_write(c, data, len);
    spin_unlock_irq(&net_lock, flags);

    if (n == 0 && c->state != TCP_ESTABLISHED && c->state != TCP_CLOSE_WAIT) {
        return -1;      /* not a full buffer, there is no connection */
    }
    return (int64_t)n;
}

int64_t net_tcp_recv(int owner, int handle, void *out, size_t max)
{
    struct tcp_conn *c = owned(owner, handle);
    if (c == NULL) {
        return -1;
    }
    uint64_t flags = spin_lock_irq(&net_lock);
    int64_t n = (int64_t)tcp_read(c, out, max);

    /* nothing to read, and nothing ever will be. */
    enum tcp_state st = c->state;
    if (n == 0 && st != TCP_CLOSE_WAIT && st != TCP_CLOSED
        && st != TCP_LAST_ACK && st != TCP_TIME_WAIT) {
        n = -1;
    }
    spin_unlock_irq(&net_lock, flags);
    return n;
}

bool net_tcp_shut(int owner, int handle)
{
    struct tcp_conn *c = owned(owner, handle);
    if (c == NULL) {
        return false;
    }
    uint64_t flags = spin_lock_irq(&net_lock);
    tcp_close(c);
    spin_unlock_irq(&net_lock, flags);
    return true;
}

bool net_tcp_give(int i_owner, int i_handle, int i_new_owner)
{
    uint64_t u_flags = spin_lock_irq(&net_lock);
    struct tcp_conn *p_conn = owned(i_owner, i_handle);
    if (p_conn != NULL) {
        p_conn->owner = i_new_owner;
    }
    spin_unlock_irq(&net_lock, u_flags);
    return p_conn != NULL;
}

int64_t net_tcp_waiting(int i_owner, int i_handle)
{
    uint64_t u_flags = spin_lock_irq(&net_lock);
    struct tcp_conn *p_conn = owned(i_owner, i_handle);
    int64_t i_waiting = (p_conn != NULL) ? (int64_t)p_conn->recv_len : -1;
    spin_unlock_irq(&net_lock, u_flags);
    return i_waiting;
}

bool net_tcp_ends(int i_owner, int i_handle, ipv4 *p_local,
                  uint16_t *p_local_port, ipv4 *p_remote,
                  uint16_t *p_remote_port)
{
    uint64_t u_flags = spin_lock_irq(&net_lock);
    struct tcp_conn *p_conn = owned(i_owner, i_handle);
    if (p_conn != NULL) {
        *p_local = p_conn->local;
        *p_local_port = p_conn->local_port;
        *p_remote = p_conn->remote;
        *p_remote_port = p_conn->remote_port;
    }
    spin_unlock_irq(&net_lock, u_flags);
    return p_conn != NULL;
}

int net_tcp_error(int i_owner, int i_handle)
{
    uint64_t u_flags = spin_lock_irq(&net_lock);
    struct tcp_conn *p_conn = owned(i_owner, i_handle);
    int i_errno = (p_conn != NULL) ? p_conn->i_errno : 0;
    spin_unlock_irq(&net_lock, u_flags);
    return i_errno;
}

int net_tcp_state(int owner, int handle)
{
    struct tcp_conn *c = owned(owner, handle);
    return c == NULL ? -1 : (int)c->state;
}

ipv4 net_router(void)
{
    return my_router;
}

bool net_send_ip(ipv4 to, uint8_t protocol, const void *payload, size_t len)
{
    if (!up || len + IP_HEADER_MIN > ETHER_MTU) {
        return false;
    }

    /* a datagram addressed to this machine's own address must never reach the wire. */
    if (to == my_ip) {
        /*
         * on the stack rather than static, unlike every other buffer in
         * this file, and for one reason: this path *re-enters itself*.
         * a loopback echo request reaches got_icmp, which answers it,
         * which comes straight back through here, and a shared buffer
         * would have the inner datagram overwrite the outer one while
         * the outer parse still holds pointers into it.
         *
         * the depth is two and cannot be more: an echo reply provokes no
         * further send, and a udp datagram reaching a socket provokes
         * none either. two of these plus got_icmp's own reply buffer is
         * about four and a half kilobytes of a sixteen kilobyte kernel
         * stack, which is worth it to not depend on that
         */
        uint8_t loop[ETHER_MTU];
        size_t at = ip_build(loop, my_ip, to, protocol, (uint16_t)len,
                             (uint16_t)(pit_uptime_ms() & 0xffff));
        memcpy(loop + at, payload, len);

        got_ip(loop, at + len);
        return true;
    }

    /* who to hand it to. an address on this wire goes straight to that machine. */
    ipv4 hop = ip_next_hop(to, my_ip, my_mask, my_router);
    if (hop == 0) {
        return false;
    }

    struct mac to_mac;
    bool known;

    uint64_t flags = spin_lock_irq(&net_lock);
    known = arp_lookup(&cache, hop, pit_uptime_ms(), &to_mac);
    spin_unlock_irq(&net_lock, flags);

    if (!known) {
        /*
         * ask, and say no. a send that blocked here would be a machine
         * that stops when one address does not answer, and the caller
         * trying again in a moment is both simpler and what every stack
         * actually does underneath.
         *
         * the question is about the *next hop*: asking this wire who
         * has 104.20.23.154 gets no answer from anybody, forever
         */
        net_arp_request(hop);
        return false;
    }

    static uint8_t datagram[ETHER_MTU];
    size_t at = ip_build(datagram, my_ip, to, protocol, (uint16_t)len,
                         (uint16_t)(pit_uptime_ms() & 0xffff));
    memcpy(datagram + at, payload, len);

    return send_frame(&to_mac, ETHERTYPE_IPV4, datagram, at + len);
}

/*
 * every one of these takes the interface's lock, because a datagram is
 * delivered on the poll thread and taken on whichever thread the program
 * happens to be. the pure half in socket.c has no idea any of that is
 * happening, which is what let it be tested against a table in memory
 */

static struct socket_table sockets;

int net_socket_open(int owner, uint16_t port)
{
    uint64_t flags = spin_lock_irq(&net_lock);
    int h = socket_open(&sockets, owner, port);
    spin_unlock_irq(&net_lock, flags);
    return h;
}

bool net_socket_close(int owner, int handle)
{
    uint64_t flags = spin_lock_irq(&net_lock);
    bool ok = socket_close(&sockets, owner, handle);
    spin_unlock_irq(&net_lock, flags);
    return ok;
}

int64_t net_socket_take(int owner, int handle, ipv4 *from,
                        uint16_t *from_port, void *out, size_t max)
{
    uint64_t flags = spin_lock_irq(&net_lock);
    int64_t n = socket_take(&sockets, owner, handle, from, from_port, out, max);
    spin_unlock_irq(&net_lock, flags);
    return n;
}

size_t net_socket_pending(int i_owner, int i_handle)
{
    uint64_t u_flags = spin_lock_irq(&net_lock);
    size_t sz_count = socket_waiting(&sockets, i_owner, i_handle);
    spin_unlock_irq(&net_lock, u_flags);
    return sz_count;
}

bool net_socket_send(int owner, int handle, ipv4 to, uint16_t port,
                     const void *data, size_t len)
{
    uint16_t mine;

    uint64_t flags = spin_lock_irq(&net_lock);
    bool ok = socket_port(&sockets, owner, handle, &mine);
    spin_unlock_irq(&net_lock, flags);

    if (!ok || len + UDP_HEADER_LEN + IP_HEADER_MIN > ETHER_MTU) {
        return false;
    }

    /* built outside the lock. */
    static uint8_t datagram[ETHER_MTU];
    size_t n = udp_build(datagram, my_ip, to, mine, port, data, len);

    return net_send_ip(to, IP_PROTO_UDP, datagram, n);
}


static void got_arp(const uint8_t *p, size_t len)
{
    struct arp_packet a;
    if (!arp_parse(p, len, &a)) {
        return;
    }

    /* learn from anything that speaks, request or reply. */
    uint64_t flags = spin_lock_irq(&net_lock);
    arp_learn(&cache, a.sender_ip, &a.sender_mac, pit_uptime_ms());
    spin_unlock_irq(&net_lock, flags);

    if (a.op == ARP_REQUEST && up && a.target_ip == my_ip) {
        uint8_t reply[ARP_PACKET_LEN];
        arp_build(reply, ARP_REPLY, e1000_mac(), my_ip,
                  &a.sender_mac, a.sender_ip);
        send_frame(&a.sender_mac, ETHERTYPE_ARP, reply, sizeof reply);
    }
}

static void got_icmp(const struct ip_header *ih, const uint8_t *p, size_t len)
{
    struct icmp_echo e;
    if (!icmp_parse_echo(p, len, &e)) {
        return;
    }

    /*
     * a request is answered, with the payload copied straight back. a
     * reply answers nothing the kernel asked, since it sends no echoes
     * of its own, and goes no further
     */
    if (e.type != ICMP_ECHO_REQUEST) {
        return;
    }
    uint8_t reply[ETHER_MTU];
    if (ICMP_HEADER_LEN + e.payload_len > sizeof reply) {
        return;
    }
    size_t n = icmp_build_echo(reply, ICMP_ECHO_REPLY, e.id, e.seq,
                               p + e.payload_at, e.payload_len);
    net_send_ip(ih->from, IP_PROTO_ICMP, reply, n);
}

static void got_ip(const uint8_t *p, size_t len)
{
    struct ip_header ih;
    if (!ip_parse(p, len, &ih)) {
        return;
    }

    /* not for this machine. */
    if (up && ih.to != my_ip && ih.to != 0xffffffff) {
        return;
    }

    const uint8_t *payload = p + ih.payload_at;
    size_t payload_len = ih.payload_len;

    if (ih.protocol == IP_PROTO_TCP) {
        /*
         * FIXME: this runs on the poll thread holding no lock, and
         * tcp_input rewrites the connection it lands on: state, sequence
         * numbers, and the receive buffer that an application may be
         * reading under net_lock at that same moment. tcp_run() below is
         * the same, ticking and sending out of the same structs. every
         * socket entry point in this file takes that lock, so the
         * lock exists and the half of the stack sharing these connections
         * is the half that does not hold it. one tcp_read copying down
         * through recv_buf while take_payload copies into it is the whole
         * failure: a stream comes back with bytes repeated or skipped.
         */
        got_tcp(ih.from, ih.to, payload, payload_len);
    } else if (ih.protocol == IP_PROTO_ICMP) {
        got_icmp(&ih, payload, payload_len);
    } else if (ih.protocol == IP_PROTO_UDP) {
        struct udp_header uh;
        if (!udp_parse(payload, payload_len, ih.from, ih.to, &uh)) {
            return;
        }
        /* and now there is somewhere for it to go. */
        uint64_t flags = spin_lock_irq(&net_lock);
        socket_deliver(&sockets, uh.to_port, ih.from, uh.from_port,
                       payload + uh.payload_at, uh.payload_len);
        spin_unlock_irq(&net_lock, flags);
    }
}

void net_receive(const uint8_t *frame, size_t len)
{
    struct ether_header eh;
    if (!ether_parse(frame, len, &eh)) {
        return;
    }

    const uint8_t *payload = frame + ETHER_HEADER_LEN;
    size_t payload_len = len - ETHER_HEADER_LEN;

    if (eh.type == ETHERTYPE_ARP) {
        got_arp(payload, payload_len);
    } else if (eh.type == ETHERTYPE_IPV4) {
        got_ip(payload, payload_len);
    }
    /*
     * and everything else on the wire is somebody else's protocol,
     * which is most of what arrives and is not an error
     */
}

/* which thread drains the card, so the interrupt has somebody to wake */
static int poll_tid = -1;

/* called from the card's interrupt handler, and therefore allowed to do almost nothing. */
static void net_wake(void)
{
    if (poll_tid >= 0) {
        sched_wake_thread(poll_tid);
    }
}

void net_service_ready(void)
{
    struct thread *me = sched_current();
    poll_tid = (me != NULL) ? me->id : -1;
    e1000_on_arrival(net_wake);
}

void net_poll(void)
{
    for (;;) {
        size_t n = e1000_receive(in_frame, sizeof in_frame);
        if (n == 0) {
            break;
        }
        net_receive(in_frame, n);
    }

    /*
     * after the frames rather than before: an acknowledgement that just
     * arrived should stop a retransmission now, not in half a second
     */
    tcp_run();
}

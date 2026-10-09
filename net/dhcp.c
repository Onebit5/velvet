// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/dhcp.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * dhcp: asking for an address.
 */

#include <velvet/dhcp.h>
#include <velvet/string.h>

/* the fixed part of a bootp message, by offset. */
#define OFF_OP      0       /* 1 = a client asking, 2 = a server telling */
#define OFF_HTYPE   1       /* 1 = ethernet */
#define OFF_HLEN    2       /* 6 */
#define OFF_HOPS    3
#define OFF_XID     4
#define OFF_SECS    8
#define OFF_FLAGS   10
#define OFF_CIADDR  12      /* what the client already has */
#define OFF_YIADDR  16      /* what the server is giving it */
#define OFF_SIADDR  20
#define OFF_GIADDR  24
#define OFF_CHADDR  28      /* 16 bytes, of which 6 are used */
#define OFF_COOKIE  236

#define BOOTREQUEST 1
#define BOOTREPLY   2

/*
 * set by a client that cannot yet receive a unicast reply, which is
 * every client that does not have an address, so, this one
 */
#define FLAG_BROADCAST 0x8000

/* the four bytes that separate dhcp from the bootp it grew out of. */
#define MAGIC_COOKIE 0x63825363u

/* the options that matter */
#define OPT_MASK        1
#define OPT_ROUTER      3
#define OPT_DNS         6
#define OPT_REQUESTED   50
#define OPT_LEASE       51
#define OPT_TYPE        53
#define OPT_SERVER      54
#define OPT_PARAMS      55
#define OPT_PAD         0
#define OPT_END         255



bool dhcp_parse(const uint8_t *p, size_t len, struct dhcp_message *out)
{
    /* the fixed part plus the cookie. */
    if (len < OFF_COOKIE + 4) {
        return false;
    }
    if (p[OFF_OP] != BOOTREPLY || p[OFF_HTYPE] != 1 || p[OFF_HLEN] != MAC_LEN) {
        return false;
    }
    if (get32be(p + OFF_COOKIE) != MAGIC_COOKIE) {
        return false;
    }

    memset(out, 0, sizeof *out);
    out->xid   = get32be(p + OFF_XID);
    out->yours = get32be(p + OFF_YIADDR);

    /* a list of type/length/value, and the length is a byte that came off the wire. */
    size_t at = OFF_COOKIE + 4;
    while (at < len) {
        uint8_t code = p[at];

        if (code == OPT_END) {
            break;
        }
        if (code == OPT_PAD) {      /* no length byte, unlike every other */
            at++;
            continue;
        }
        if (at + 1 >= len) {
            return false;           /* a code with no length after it */
        }
        uint8_t olen = p[at + 1];
        const uint8_t *v = p + at + 2;
        if (at + 2 + olen > len) {
            return false;           /* a length that runs off the end */
        }

        switch (code) {
        case OPT_TYPE:
            if (olen == 1) { out->type = v[0]; }
            break;
        case OPT_MASK:
            if (olen == 4) { out->mask = get32be(v); }
            break;
        case OPT_ROUTER:
            /* a list, and the first is the one used. */
            if (olen >= 4) { out->router = get32be(v); }
            break;
        case OPT_DNS:
            if (olen >= 4) { out->dns = get32be(v); }
            break;
        case OPT_LEASE:
            if (olen == 4) { out->lease_s = get32be(v); }
            break;
        case OPT_SERVER:
            /*
             * option 54 rather than siaddr, which is the same thing in
             * theory and often empty in practice, siaddr is bootp's
             * "the server to load your image from" and a dhcp server
             * with no images to hand out has no reason to fill it in.
             * the request has to be addressed to whoever actually made
             * the offer, so it is this one that counts
             */
            if (olen == 4) { out->server = get32be(v); }
            break;
        default:
            break;                  /* most of them, and none an error */
        }
        at += 2 + olen;
    }

    /* no type means bootp, or a truncated message that happened to survive the bounds checks. */
    return out->type != 0;
}



static size_t put_opt(uint8_t *p, size_t at, uint8_t code,
                      const void *value, uint8_t len)
{
    p[at++] = code;
    p[at++] = len;
    memcpy(p + at, value, len);
    return at + len;
}

static size_t put_opt32(uint8_t *p, size_t at, uint8_t code, uint32_t v)
{
    uint8_t b[4];
    put32be(b, v);
    return put_opt(p, at, code, b, 4);
}

/*
 * build one message. `ciaddr` is what this machine already has, which is
 * zero for everything except a renewal, and a renewal is the one case
 * where the server needs to know which lease is being talked about
 * without being told twice
 */
static size_t build(uint8_t *p, size_t max, const struct dhcp *d,
                    uint8_t type, ipv4 ciaddr,
                    ipv4 requested, ipv4 server)
{
    if (max < DHCP_MIN_LEN) {
        return 0;
    }
    memset(p, 0, DHCP_MIN_LEN);

    p[OFF_OP]    = BOOTREQUEST;
    p[OFF_HTYPE] = 1;
    p[OFF_HLEN]  = MAC_LEN;
    put32be(p + OFF_XID, d->xid);

    /* "answer the kernel by broadcast". */
    put16be(p + OFF_FLAGS, ciaddr == 0 ? FLAG_BROADCAST : 0);

    put32be(p + OFF_CIADDR, ciaddr);
    memcpy(p + OFF_CHADDR, d->me.b, MAC_LEN);
    put32be(p + OFF_COOKIE, MAGIC_COOKIE);

    size_t at = OFF_COOKIE + 4;
    at = put_opt(p, at, OPT_TYPE, &type, 1);

    if (requested != 0) {
        at = put_opt32(p, at, OPT_REQUESTED, requested);
    }
    if (server != 0) {
        at = put_opt32(p, at, OPT_SERVER, server);
    }

    /* what this machine would like to be told. */
    static const uint8_t wanted[] = { OPT_MASK, OPT_ROUTER, OPT_DNS };
    at = put_opt(p, at, OPT_PARAMS, wanted, sizeof wanted);

    p[at++] = OPT_END;

    /* padded out rather than sent at its real length. */
    return at < DHCP_MIN_LEN ? DHCP_MIN_LEN : at;
}



void dhcp_start(struct dhcp *d, const struct mac *me, uint32_t xid,
                uint64_t now_ms)
{
    memset(d, 0, sizeof *d);
    d->me    = *me;
    d->xid   = xid;
    d->state = DHCP_SELECTING;

    /*
     * sent_ms in the past by a whole wait, so the first tick sends
     * immediately rather than after a second. the alternative is a
     * special case for "has anything been sent yet", and a state
     * machine with a flag meaning "not really in this state yet" is how
     * these become impossible to follow
     */
    d->sent_ms = now_ms - DHCP_FIRST_WAIT;
    d->tries   = 0;
}

void dhcp_stop(struct dhcp *d)
{
    /* no release is sent, deliberately. */
    d->state = DHCP_OFF;
}

/* how long to wait after the `tries`th message before repeating it: 1, 2, 4 seconds. */
static uint64_t wait_for(unsigned tries)
{
    uint64_t w = DHCP_FIRST_WAIT;
    for (unsigned i = 1; i < tries && i < DHCP_TRIES; i++) {
        w *= 2;
    }
    return w;
}

void dhcp_input(struct dhcp *d, const uint8_t *p, size_t len,
                uint64_t now_ms)
{
    if (d->state == DHCP_OFF || d->state == DHCP_FAILED) {
        return;
    }

    struct dhcp_message m;
    if (!dhcp_parse(p, len, &m)) {
        return;
    }

    /* somebody else's conversation. */
    if (m.xid != d->xid) {
        return;
    }

    switch (d->state) {
    case DHCP_SELECTING:
        if (m.type != DHCP_OFFER || m.yours == 0) {
            return;
        }
        d->offers++;

        /* the first offer wins. */
        d->address = m.yours;
        d->server  = m.server;
        d->mask    = m.mask;
        d->router  = m.router;
        d->dns     = m.dns;

        d->state   = DHCP_REQUESTING;
        d->tries   = 0;
        d->sent_ms = now_ms - wait_for(0);   /* request goes out at once */
        break;

    case DHCP_REQUESTING:
    case DHCP_RENEWING:
        if (m.type == DHCP_NAK) {
            /*
             * the server changed its mind, which happens when a lease
             * was given to somebody else in between, or when a renewal
             * is refused because the machine moved to another wire.
             * either way what this machine believes is wrong, and the
             * only correct move is to forget all of it and start over
             *, keeping the address and retrying would be arguing
             */
            d->naks++;
            d->address = d->server = d->mask = d->router = d->dns = 0;
            d->lease_s = 0;
            d->state   = DHCP_SELECTING;
            d->tries   = 0;
            d->sent_ms = now_ms - wait_for(0);
            return;
        }
        if (m.type != DHCP_ACK) {
            return;
        }
        d->acks++;

        /* the ack is what counts, not the offer. */
        d->address = m.yours != 0 ? m.yours : d->address;
        if (m.mask   != 0) { d->mask   = m.mask; }
        if (m.router != 0) { d->router = m.router; }
        if (m.dns    != 0) { d->dns    = m.dns; }
        if (m.server != 0) { d->server = m.server; }

        /* a lease of zero would be renewed immediately and forever. */
        d->lease_s   = m.lease_s != 0 ? m.lease_s : 3600;
        d->bound_ms  = now_ms;
        d->state     = DHCP_BOUND;
        d->tries     = 0;
        break;

    default:
        break;                  /* bound: nothing arriving matters */
    }
}

size_t dhcp_tick(struct dhcp *d, uint64_t now_ms, uint8_t *out, size_t max)
{
    switch (d->state) {
    case DHCP_OFF:
    case DHCP_FAILED:
        return 0;

    case DHCP_SELECTING:
    case DHCP_REQUESTING: {
        if (now_ms - d->sent_ms < wait_for(d->tries)) {
            return 0;
        }
        if (d->tries >= DHCP_TRIES) {
            /* nobody is out there, or nobody will say yes. */
            d->state = DHCP_FAILED;
            return 0;
        }
        d->sent_ms = now_ms;
        d->tries++;

        if (d->state == DHCP_SELECTING) {
            d->discovers++;
            return build(out, max, d, DHCP_DISCOVER, 0, 0, 0);
        }
        d->requests++;
        /*
         * ciaddr stays zero here even though an address has been
         * offered: it is not this machine's until the ack, and a client
         * that fills it in before then is claiming something it was not
         * given. the offered address goes in option 50 instead, which
         * is what that option is for
         */
        return build(out, max, d, DHCP_REQUEST, 0, d->address, d->server);
    }

    case DHCP_BOUND: {
        /* renew at half the lease. */
        uint64_t half = (uint64_t)d->lease_s * 1000 / 2;
        if (now_ms - d->bound_ms < half) {
            return 0;
        }
        d->state   = DHCP_RENEWING;
        d->tries   = 1;
        d->sent_ms = now_ms;
        d->requests++;
        /*
         * addressed to the server that granted it, from the address
         * this machine holds, so ciaddr is filled in and there is no
         * option 50. this is the one message here that is not a
         * broadcast, and the caller sends it to d->server
         */
        return build(out, max, d, DHCP_REQUEST, d->address, 0, 0);
    }

    case DHCP_RENEWING: {
        if (now_ms - d->sent_ms < wait_for(d->tries)) {
            return 0;
        }
        /* the lease is the deadline, not the try count. */
        if (dhcp_lease_left(d, now_ms) == 0) {
            d->address = d->server = d->mask = 0;
            d->lease_s = 0;
            d->state   = DHCP_SELECTING;
            d->tries   = 0;
            d->sent_ms = now_ms - wait_for(0);
            return 0;
        }
        d->sent_ms = now_ms;
        d->tries++;
        d->requests++;
        return build(out, max, d, DHCP_REQUEST, d->address, 0, 0);
    }
    }
    return 0;
}

bool dhcp_take_address(struct dhcp *d)
{
    /* what the caller has already been given, rather than a "there is something new" flag. */
    if (d->state != DHCP_BOUND || d->address == 0) {
        return false;
    }
    if (d->address == d->applied) {
        return false;
    }
    d->applied = d->address;
    return true;
}

uint32_t dhcp_lease_left(const struct dhcp *d, uint64_t now_ms)
{
    if (d->lease_s == 0) {
        return 0;
    }
    uint64_t gone = (now_ms - d->bound_ms) / 1000;
    return gone >= d->lease_s ? 0 : (uint32_t)(d->lease_s - gone);
}

const char *dhcp_state_name(const struct dhcp *d)
{
    switch (d->state) {
    case DHCP_OFF:        return "off";
    case DHCP_SELECTING:  return "looking for a server";
    case DHCP_REQUESTING: return "asking for the address it was offered";
    case DHCP_BOUND:      return "bound";
    case DHCP_RENEWING:   return "renewing";
    case DHCP_FAILED:     return "nobody answered";
    }
    return "?";
}

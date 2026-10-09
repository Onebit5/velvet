// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/dhcp.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * dhcp: an address somebody else decides.
 */

/* the design notes for dhcp.h are in docs/subsystems/mm.rst */

#ifndef VELVET_DHCP_H
#define VELVET_DHCP_H

#include <velvet/net.h>

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

#define DHCP_DISCOVER 1
#define DHCP_OFFER    2
#define DHCP_REQUEST  3
#define DHCP_DECLINE  4
#define DHCP_ACK      5
#define DHCP_NAK      6
#define DHCP_RELEASE  7

#define DHCP_FIXED_LEN 236
#define DHCP_MIN_LEN   300
#define DHCP_MAX_LEN   576      /* what every server must accept */

struct dhcp_message {
    uint8_t  type;              /* option 53 */
    uint32_t xid;
    ipv4     yours;             /* yiaddr: the address being offered */
    ipv4     server;            /* option 54, not siaddr, see the note
                                 * in dhcp.c, they disagree in practice */
    ipv4     mask;              /* option 1 */
    ipv4     router;            /* option 3, 0 if not offered */
    ipv4     dns;               /* option 6, first one only */
    uint32_t lease_s;           /* option 51 */
};

/*
 * false on anything that is not a well-formed reply to a client: too
 * short, wrong op, no magic cookie, no message type. an option that
 * runs off the end of the buffer is the one worth being careful about,
 * since options are length-prefixed and the length comes off the wire
 */
bool dhcp_parse(const uint8_t *p, size_t len, struct dhcp_message *out);

enum dhcp_state {
    DHCP_OFF,           /* not asking */
    DHCP_SELECTING,     /* discover sent, waiting for an offer */
    DHCP_REQUESTING,    /* offer taken, waiting for the ack */
    DHCP_BOUND,         /* has an address and a lease */
    DHCP_RENEWING,      /* has one, and has asked to keep it */
    DHCP_FAILED         /* nobody answered. see `tries` */
};

#define DHCP_TRIES      4
#define DHCP_FIRST_WAIT 1000

struct dhcp {
    enum dhcp_state state;
    uint32_t xid;               /* this conversation's number */
    struct mac me;

    ipv4 address, server, mask, router, dns;
    uint32_t lease_s;

    uint64_t sent_ms;
    unsigned tries;

    uint64_t bound_ms;

    /*
     * the address the caller has already configured the interface with,
     * which is not always the one held: a renewal grants the same
     * address again and must not count as something new. in the struct
     * rather than a static inside dhcp_take_address, which is where it
     * started, a file-static could not be reset between two tests, so
     * the second would silently inherit the first's answer
     */
    ipv4 applied;

    unsigned discovers, offers, requests, acks, naks;
};

void dhcp_start(struct dhcp *d, const struct mac *me, uint32_t xid,
                uint64_t now_ms);

/* give it up and go quiet. does not send a release, see dhcp.c */
void dhcp_stop(struct dhcp *d);

void dhcp_input(struct dhcp *d, const uint8_t *p, size_t len,
                uint64_t now_ms);

size_t dhcp_tick(struct dhcp *d, uint64_t now_ms, uint8_t *out, size_t max);

bool dhcp_take_address(struct dhcp *d);

const char *dhcp_state_name(const struct dhcp *d);

uint32_t dhcp_lease_left(const struct dhcp *d, uint64_t now_ms);

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/dns.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * dns: turning a word into an address.
 */

/* the design notes for dns.h are in docs/subsystems/mm.rst */

#ifndef VELVET_DNS_H
#define VELVET_DNS_H

#include <velvet/net.h>

/*
 * dns: turning a word into an address, and the first time this machine
 * asks a question of software somebody else wrote years ago, whose only
 * sensible posture is to believe none of it.
 *
 * a header of twelve bytes, then a question, then answers. a name is
 * length-prefixed labels ending in a zero byte, so the dots in a name are
 * exactly the label boundaries.
 *
 * compression is the whole of the danger. a name may be cut short by a
 * pointer whose remaining fourteen bits are an offset into the message,
 * and a pointer may point backwards, forwards or at itself, so a decoder
 * that simply follows them can be hung forever by a fourteen-byte reply,
 * and one can be made to point into the middle of a length byte so that a
 * name decodes out of a region that was never a name. so every jump is
 * counted and bounded, every offset is checked against the message before
 * it is followed, and a name that has not ended when the budget runs out
 * is refused rather than truncated, since a truncated name resolves to
 * *something* and something is what gets connected to.
 *
 * no recursion of its own, the question is asked with the recursion bit
 * set and a real resolver does the walking. no AAAA, no TXT, no MX, no
 * zone transfers, and one question per query
 */

#define DNS_PORT 53

#define DNS_NAME_MAX  255
#define DNS_LABEL_MAX 63

#define DNS_MESSAGE_MAX 512

#define DNS_HEADER_LEN 12

#define DNS_TYPE_A     1
#define DNS_TYPE_CNAME 5
#define DNS_CLASS_IN   1

enum dns_result {
    DNS_OK,
    DNS_NO_SUCH_NAME,   /* rcode 3: it does not exist */
    DNS_NO_ADDRESS,     /* it exists and has no A record */
    DNS_REFUSED,        /* the server would not answer */
    DNS_MALFORMED       /* and this one is not the server's fault */
};

size_t dns_build_query(uint8_t *p, size_t max, const char *name, uint16_t id);

enum dns_result dns_parse_response(const uint8_t *p, size_t len,
                                   uint16_t id, const char *name,
                                   ipv4 *out, uint32_t *ttl);

bool dns_read_name(const uint8_t *p, size_t len, size_t at,
                   char *out, size_t max, size_t *after);

#endif

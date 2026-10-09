// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/dns.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * dns: turning a name into an address.
 */

#include <velvet/dns.h>
#include <velvet/string.h>

/* the two top bits of a length byte. */
#define LABEL_POINTER 0xc0
#define LABEL_MASK    0x3f

/* how many pointers one name may follow before it is called a cycle. */
#define MAX_JUMPS 8



bool dns_read_name(const uint8_t *p, size_t len, size_t at,
                   char *out, size_t max, size_t *after)
{
    size_t written = 0;
    unsigned jumps = 0;
    bool jumped = false;

    if (max == 0) {
        return false;
    }

    for (;;) {
        if (at >= len) {
            return false;               /* off the end of the message */
        }
        uint8_t n = p[at];

        if ((n & LABEL_POINTER) == LABEL_POINTER) {
            if (at + 1 >= len) {
                return false;           /* half a pointer */
            }
            size_t target = (size_t)((n & LABEL_MASK) << 8) | p[at + 1];

            /*
             * where the name *after* this one continues: a pointer ends
             * the name it appears in, so this is settled by the first
             * jump and never moves again however many follow
             */
            if (!jumped) {
                if (after != NULL) {
                    *after = at + 2;
                }
                jumped = true;
            }
            if (++jumps > MAX_JUMPS) {
                return false;           /* a cycle, or a construction */
            }
            /*
             * a target outside the message is refused by the bound at
             * the top of this loop, which every other read here goes
             * through as well. there was a second check here saying the
             * same thing; break-testing it showed it could not fail
             * independently, and two checks for one condition is one
             * that will eventually be edited alone
             */
            at = target;
            continue;
        }

        if (n == 0) {                   /* the root label ends the name */
            if (!jumped && after != NULL) {
                *after = at + 1;
            }
            out[written] = '\0';
            return true;
        }

        /* and this one bound covers three separate things worth naming. */
        if (n > DNS_LABEL_MAX || at + 1 + n > len) {
            return false;
        }

        /* the dot goes *between* labels, so it is written before every one but the first. */
        if (written > 0) {
            if (written + 1 >= max) {
                return false;
            }
            out[written++] = '.';
        }
        if (written + n >= max || written + n > DNS_NAME_MAX) {
            return false;
        }
        for (uint8_t i = 0; i < n; i++) {
            out[written++] = (char)p[at + 1 + i];
        }
        at += 1 + n;
    }
}

/* write a name in wire form. returns 0 on anything that is not one */
static size_t write_name(uint8_t *p, size_t max, const char *name)
{
    size_t at = 0;

    if (name[0] == '\0') {
        return 0;
    }

    const char *label = name;
    for (;;) {
        /* to the next dot, or to the end */
        size_t n = 0;
        while (label[n] != '\0' && label[n] != '.') {
            n++;
        }
        if (n == 0 || n > DNS_LABEL_MAX) {
            /* an empty label is "a..b" or a trailing dot on nothing. */
            return 0;
        }
        if (at + 1 + n >= max || at + 1 + n > DNS_NAME_MAX) {
            return 0;
        }
        p[at++] = (uint8_t)n;
        for (size_t i = 0; i < n; i++) {
            p[at++] = (uint8_t)label[i];
        }

        label += n;
        if (*label == '\0') {
            break;
        }
        label++;                        /* past the dot */
        if (*label == '\0') {
            break;                      /* a trailing dot: the root */
        }
    }

    if (at + 1 > max) {
        return 0;
    }
    p[at++] = 0;
    return at;
}

/* step over a name without decoding it, for the records the project do not want */
static bool skip_name(const uint8_t *p, size_t len, size_t at, size_t *after)
{
    char scratch[DNS_NAME_MAX + 1];
    return dns_read_name(p, len, at, scratch, sizeof scratch, after);
}



size_t dns_build_query(uint8_t *p, size_t max, const char *name,
                       uint16_t id)
{
    if (max < DNS_HEADER_LEN + 5) {
        return 0;
    }

    put16be(p,     id);
    put16be(p + 2, 0x0100);     /* a question, with recursion desired */
    put16be(p + 4, 1);          /* one question */
    put16be(p + 6, 0);
    put16be(p + 8, 0);
    put16be(p + 10, 0);

    size_t n = write_name(p + DNS_HEADER_LEN, max - DNS_HEADER_LEN, name);
    if (n == 0) {
        return 0;
    }
    size_t at = DNS_HEADER_LEN + n;
    if (at + 4 > max) {
        return 0;
    }
    put16be(p + at,     DNS_TYPE_A);
    put16be(p + at + 2, DNS_CLASS_IN);
    return at + 4;
}



enum dns_result dns_parse_response(const uint8_t *p, size_t len,
                                   uint16_t id, const char *name,
                                   ipv4 *out, uint32_t *ttl)
{
    if (len < DNS_HEADER_LEN) {
        return DNS_MALFORMED;
    }

    /*
     * the id first. this is the only thing tying a reply to a question,
     * and checking it is what stops any packet that happens to arrive on
     * the right port from being taken as the answer
     */
    if (get16be(p) != id) {
        return DNS_MALFORMED;
    }

    uint16_t flags = get16be(p + 2);
    if ((flags & 0x8000) == 0) {
        return DNS_MALFORMED;           /* a question, not an answer */
    }

    uint8_t rcode = (uint8_t)(flags & 0x000f);
    if (rcode == 3) {
        return DNS_NO_SUCH_NAME;
    }
    if (rcode != 0) {
        return DNS_REFUSED;
    }

    uint16_t questions = get16be(p + 4);
    uint16_t answers   = get16be(p + 6);

    size_t at = DNS_HEADER_LEN;

    /* the question, echoed back. */
    if (questions != 1) {
        return DNS_MALFORMED;
    }
    {
        char asked[DNS_NAME_MAX + 1];
        if (!dns_read_name(p, len, at, asked, sizeof asked, &at)) {
            return DNS_MALFORMED;
        }
        if (strcasecmp(asked, name) != 0) {
            return DNS_MALFORMED;
        }
        if (at + 4 > len) {
            return DNS_MALFORMED;
        }
        at += 4;                        /* type and class */
    }

    /* walked in order, taking the first A record. */
    for (uint16_t i = 0; i < answers; i++) {
        if (!skip_name(p, len, at, &at)) {
            return DNS_MALFORMED;
        }
        if (at + 10 > len) {
            return DNS_MALFORMED;
        }
        uint16_t type   = get16be(p + at);
        uint16_t class  = get16be(p + at + 2);
        uint32_t record_ttl = get32be(p + at + 4);
        uint16_t rdlen  = get16be(p + at + 8);
        at += 10;

        if (at + rdlen > len) {
            return DNS_MALFORMED;       /* claiming more than arrived */
        }

        if (type == DNS_TYPE_A && class == DNS_CLASS_IN) {
            if (rdlen != 4) {
                /* an A record is four bytes. */
                return DNS_MALFORMED;
            }
            if (out != NULL) {
                *out = get32be(p + at);
            }
            if (ttl != NULL) {
                *ttl = record_ttl;
            }
            return DNS_OK;
        }
        at += rdlen;
    }

    /* it exists, the server said so, with rcode 0, and there is no address in the reply. */
    return DNS_NO_ADDRESS;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/socket.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * sockets: a handle, and the table behind it.
 */

#include <velvet/socket.h>
#include <velvet/string.h>

void socket_table_reset(struct socket_table *t)
{
    memset(t, 0, sizeof *t);
}

/*
 * a handle is an index, and 0 is a perfectly good one, so "no socket"
 * has to be -1 rather than 0, and every check below says so explicitly
 */
static struct socket *at(struct socket_table *t, int owner, int handle)
{
    if (handle < 0 || handle >= SOCKET_MAX) {
        return NULL;
    }
    struct socket *s = &t->s[handle];
    if (!s->used || s->owner != owner) {
        /* somebody else's socket, or none. */
        return NULL;
    }
    return s;
}

int socket_open(struct socket_table *t, int owner, uint16_t port)
{
    if (port == 0) {
        return -1;              /* port zero is reserved everywhere */
    }
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (t->s[i].used && t->s[i].port == port) {
            /*
             * two programs on one port would mean a datagram going to
             * whichever was found first, which is worse than refusing
             */
            return -1;
        }
    }
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (t->s[i].used) {
            continue;
        }
        memset(&t->s[i], 0, sizeof t->s[i]);
        t->s[i].used  = true;
        t->s[i].owner = owner;
        t->s[i].port  = port;
        return i;
    }
    return -1;
}

bool socket_close(struct socket_table *t, int owner, int handle)
{
    struct socket *s = at(t, owner, handle);
    if (s == NULL) {
        return false;
    }
    memset(s, 0, sizeof *s);
    return true;
}

void socket_close_all(struct socket_table *t, int owner)
{
    for (int i = 0; i < SOCKET_MAX; i++) {
        if (t->s[i].used && t->s[i].owner == owner) {
            memset(&t->s[i], 0, sizeof t->s[i]);
        }
    }
}

int socket_deliver(struct socket_table *t, uint16_t to_port, ipv4 from,
                   uint16_t from_port, const void *data, size_t len)
{
    if (len > SOCKET_DATAGRAM) {
        return -1;
    }

    for (int i = 0; i < SOCKET_MAX; i++) {
        struct socket *s = &t->s[i];
        if (!s->used || s->port != to_port) {
            continue;
        }

        if (s->count == SOCKET_QUEUE) {
            /* the queue is full and this one is lost. */
            s->dropped++;

            /* the socket is still named, even though nothing was queued. */
            return i;
        }

        size_t slot = (s->head + s->count) % SOCKET_QUEUE;
        struct socket_datagram *d = &s->q[slot];
        d->from = from;
        d->from_port = from_port;
        /*
         * NOTE: the cast is honest because sys_sendto refuses a length over
         * SOCKET_DATAGRAM, which is what the slot holds.
         */
        d->len = (uint16_t)len;
        if (len > 0) {
            memcpy(d->data, data, len);
        }
        s->count++;
        return i;
    }
    return -1;          /* nobody is listening on that port, which is
                         * ordinary rather than an error */
}

int64_t socket_take(struct socket_table *t, int owner, int handle,
                    ipv4 *from, uint16_t *from_port, void *out, size_t max)
{
    struct socket *s = at(t, owner, handle);
    if (s == NULL) {
        return -1;
    }
    if (s->count == 0) {
        return -1;              /* nothing yet, which is not a failure */
    }

    struct socket_datagram *d = &s->q[s->head];
    size_t n = d->len;
    if (n > max) {
        n = max;                /* a datagram longer than the buffer is
                                 * cut rather than held back, the way
                                 * recvfrom has always behaved */
    }
    if (n > 0) {
        memcpy(out, d->data, n);
    }
    if (from != NULL) {
        *from = d->from;
    }
    if (from_port != NULL) {
        *from_port = d->from_port;
    }

    s->head = (s->head + 1) % SOCKET_QUEUE;
    s->count--;
    return (int64_t)n;
}

bool socket_port(const struct socket_table *t, int owner, int handle,
                 uint16_t *out)
{
    if (handle < 0 || handle >= SOCKET_MAX) {
        return false;
    }
    const struct socket *s = &t->s[handle];
    if (!s->used || s->owner != owner) {
        return false;
    }
    *out = s->port;
    return true;
}

size_t socket_waiting(const struct socket_table *t, int owner, int handle)
{
    if (handle < 0 || handle >= SOCKET_MAX) {
        return 0;
    }
    const struct socket *s = &t->s[handle];
    if (!s->used || s->owner != owner) {
        return 0;
    }
    return s->count;
}

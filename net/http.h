// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/http.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * enough http to pull a file down.
 */

/* the design notes for http.h are in docs/subsystems/mm.rst */

#ifndef NET_HTTP_H
#define NET_HTTP_H

#include "net/net.h"

#define HTTP_PORT 80

#define HTTP_LINE_MAX 512

#define HTTP_VALUE_MAX 256

enum http_state {
    HTTP_STATUS,        /* reading "HTTP/1.1 200 OK" */
    HTTP_HEADERS,       /* reading header lines until a blank one */
    HTTP_BODY,          /* a body of known length */
    HTTP_CHUNK_SIZE,    /* reading a hexadecimal chunk length */
    HTTP_CHUNK_DATA,    /* reading that many bytes */
    HTTP_CHUNK_END,     /* the crlf after a chunk */
    HTTP_TRAILERS,      /* headers after the last chunk, ignored */
    HTTP_EOF,           /* the body ends when the connection does */
    HTTP_DONE,
    HTTP_BROKEN
};

struct http {
    enum http_state state;

    int      status;            /* 200, 404, ... */
    bool     chunked;
    bool     has_length;
    uint64_t length;            /* what is left of the body, or chunk */
    uint64_t received;          /* body bytes produced so far */

    char   line[HTTP_LINE_MAX];
    size_t line_len;
    bool   line_too_long;

    char content_type[HTTP_VALUE_MAX];
    char location[HTTP_VALUE_MAX];
};

void http_start(struct http *h);

/*
 * build a GET. `host` goes in the Host header, which http/1.1 requires
 * and without which a server sharing an address with others cannot know
 * which site was meant
 */
size_t http_get(char *out, size_t max, const char *host, const char *path);

size_t http_feed(struct http *h, const uint8_t *in, size_t len,
                 uint8_t *out, size_t max, size_t *out_len);

void http_closed(struct http *h);

bool http_parse_url(const char *url, char *host, size_t host_max,
                    uint16_t *port, char *path, size_t path_max,
                    bool *unsupported_scheme);

#endif

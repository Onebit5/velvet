// SPDX-License-Identifier: GPL-2.0-only
/*
 * net/http.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * enough http to pull a file down.
 */

#include <velvet/http.h>
#include <velvet/string.h>



static bool starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix != '\0') {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') { a = (char)(a + 32); }
        if (b >= 'A' && b <= 'Z') { b = (char)(b + 32); }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static void copy_into(char *out, size_t max, const char *from)
{
    size_t n = 0;
    while (from[n] != '\0' && n + 1 < max) {
        out[n] = from[n];
        n++;
    }
    out[n] = '\0';
}

/*
 * a header value starts after the colon, past any spaces, and ends
 * before any trailing ones. servers are inconsistent about all three
 */
static const char *value_of(const char *line)
{
    const char *colon = line;
    while (*colon != '\0' && *colon != ':') {
        colon++;
    }
    if (*colon != ':') {
        return NULL;
    }
    colon++;
    while (*colon == ' ' || *colon == '\t') {
        colon++;
    }
    return colon;
}



bool http_parse_url(const char *url, char *host, size_t host_max,
                    uint16_t *port, char *path, size_t path_max,
                    bool *unsupported_scheme)
{
    if (unsupported_scheme != NULL) {
        *unsupported_scheme = false;
    }
    if (url == NULL || url[0] == '\0') {
        return false;
    }

    *port = HTTP_PORT;

    const char *at = url;
    if (starts_with_ci(at, "http://")) {
        at += 7;
    } else if (starts_with_ci(at, "https://")) {
        /* said separately from "not a url". */
        if (unsupported_scheme != NULL) {
            *unsupported_scheme = true;
        }
        return false;
    } else {
        /*
         * no scheme at all is taken as http, so `fetch example.com/x`
         * works the way anybody would expect it to
         */
        const char *scheme = at;
        while (*scheme != '\0' && *scheme != '/' && *scheme != ':') {
            scheme++;
        }
        if (scheme[0] == ':' && scheme[1] == '/' && scheme[2] == '/') {
            if (unsupported_scheme != NULL) {
                *unsupported_scheme = true;
            }
            return false;       /* some other scheme entirely */
        }
    }

    /* the host runs to a colon, a slash, or the end */
    size_t n = 0;
    while (at[n] != '\0' && at[n] != ':' && at[n] != '/') {
        n++;
    }
    if (n == 0 || n + 1 > host_max) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        host[i] = at[i];
    }
    host[n] = '\0';
    at += n;

    if (*at == ':') {
        at++;
        uint32_t v = 0;
        if (*at < '0' || *at > '9') {
            return false;
        }
        while (*at >= '0' && *at <= '9') {
            v = v * 10 + (uint32_t)(*at - '0');
            if (v > 65535) {
                return false;
            }
            at++;
        }
        if (v == 0) {
            return false;
        }
        *port = (uint16_t)v;
    }

    if (*at == '\0') {
        copy_into(path, path_max, "/");
        return true;
    }
    if (*at != '/') {
        return false;
    }
    copy_into(path, path_max, at);
    return true;
}



size_t http_get(char *out, size_t max, const char *host, const char *path)
{
    /* http/1.0 with an explicit Host. */
    size_t at = 0;
    const char *parts[] = {
        "GET ", path, " HTTP/1.0\r\nHost: ", host,
        "\r\nUser-Agent: velvet\r\nConnection: close\r\n\r\n"
    };

    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        const char *s = parts[i];
        while (*s != '\0') {
            if (at + 1 >= max) {
                return 0;
            }
            out[at++] = *s++;
        }
    }
    out[at] = '\0';
    return at;
}



void http_start(struct http *h)
{
    memset(h, 0, sizeof *h);
    h->state = HTTP_STATUS;
}

static void begin_body(struct http *h)
{
    if (h->chunked) {
        h->state = HTTP_CHUNK_SIZE;
        h->line_len = 0;
    } else if (h->has_length)
{
        h->state = (h->length == 0) ? HTTP_DONE : HTTP_BODY;
    } else {
        /* no length and not chunked: it ends when the connection does. */
        h->state = HTTP_EOF;
    }
}

static void got_status(struct http *h)
{
    /*
     * "HTTP/1.1 200 OK". the version is not checked beyond the prefix,
     * a server answering 1.0 to a 1.0 request is ordinary
     */
    if (!starts_with_ci(h->line, "HTTP/")) {
        h->state = HTTP_BROKEN;
        return;
    }
    const char *sp = h->line;
    while (*sp != '\0' && *sp != ' ') {
        sp++;
    }
    while (*sp == ' ') {
        sp++;
    }
    if (sp[0] < '0' || sp[0] > '9') {
        h->state = HTTP_BROKEN;
        return;
    }
    int code = 0;
    for (int i = 0; i < 3; i++) {
        if (sp[i] < '0' || sp[i] > '9') {
            h->state = HTTP_BROKEN;
            return;
        }
        code = code * 10 + (sp[i] - '0');
    }
    h->status = code;
    h->state  = HTTP_HEADERS;
}

static void got_header(struct http *h)
{
    if (h->line_len == 0) {
        begin_body(h);
        return;
    }

    const char *v = value_of(h->line);
    if (v == NULL) {
        /* a header with no colon. */
        return;
    }

    if (starts_with_ci(h->line, "content-length:")) {
        uint64_t n = 0;
        bool any = false;
        for (const char *p = v; *p >= '0' && *p <= '9'; p++) {
            /*
             * a length that could not fit in a file is refused rather
             * than wrapped round into a small one
             */
            if (n > (uint64_t)1 << 40) {
                h->state = HTTP_BROKEN;
                return;
            }
            n = n * 10 + (uint64_t)(*p - '0');
            any = true;
        }
        if (any) {
            h->has_length = true;
            h->length = n;
        }
    } else if (starts_with_ci(h->line, "transfer-encoding:")) {
        /*
         * the value may be a list; chunked is the last one if present,
         * and the only one this understands
         */
        for (const char *p = v; *p != '\0'; p++) {
            if (starts_with_ci(p, "chunked")) {
                h->chunked = true;
                break;
            }
        }
    } else if (starts_with_ci(h->line, "content-type:")) {
        copy_into(h->content_type, sizeof h->content_type, v);
    } else if (starts_with_ci(h->line, "location:")) {
        copy_into(h->location, sizeof h->location, v);
    }
}

static void got_chunk_size(struct http *h)
{
    uint64_t n = 0;
    const char *p = h->line;

    if (*p == '\0') {
        /* an empty line where a size was due. */
        return;
    }

    bool any = false;
    for (;;) {
        int d;
        if (*p >= '0' && *p <= '9')      { d = *p - '0'; }
        else if (*p >= 'a' && *p <= 'f') { d = *p - 'a' + 10; }
        else if (*p >= 'A' && *p <= 'F') { d = *p - 'A' + 10; }
        else { break; }

        if (n > ((uint64_t)1 << 36)) {
            h->state = HTTP_BROKEN;     /* a chunk nothing could hold */
            return;
        }
        n = n * 16 + (uint64_t)d;
        any = true;
        p++;
    }
    if (!any) {
        h->state = HTTP_BROKEN;
        return;
    }

    /*
     * anything after the size is a chunk extension, ";name=value",
     * and is ignored, which is what the spec asks for
     */

    if (n == 0) {
        h->state = HTTP_TRAILERS;
        h->line_len = 0;
        return;
    }
    h->length = n;
    h->state  = HTTP_CHUNK_DATA;
}

/* hand one completed line to whichever state wanted it */
static void got_line(struct http *h)
{
    h->line[h->line_len < HTTP_LINE_MAX ? h->line_len : HTTP_LINE_MAX - 1] = '\0';

    if (h->line_too_long) {
        /*
         * a line longer than the buffer was never assembled, so acting
         * on the part that fitted would be acting on a header nobody
         * sent. the status line is fatal; anything else is dropped
         */
        h->line_too_long = false;
        h->line_len = 0;
        if (h->state == HTTP_STATUS) {
            h->state = HTTP_BROKEN;
        }
        return;
    }

    switch (h->state) {
    case HTTP_STATUS:     got_status(h); break;
    case HTTP_HEADERS:    got_header(h); break;
    case HTTP_CHUNK_SIZE: got_chunk_size(h); break;
    case HTTP_TRAILERS:
        /* headers after the last chunk. */
        if (h->line_len == 0) {
            h->state = HTTP_DONE;
        }
        break;
    default:
        break;
    }
    h->line_len = 0;
}

size_t http_feed(struct http *h, const uint8_t *in, size_t len,
                 uint8_t *out, size_t max, size_t *out_len)
{
    size_t used = 0;
    size_t produced = 0;

    while (used < len) {
        if (h->state == HTTP_DONE || h->state == HTTP_BROKEN) {
            break;
        }


        if (h->state == HTTP_STATUS || h->state == HTTP_HEADERS
         || h->state == HTTP_CHUNK_SIZE || h->state == HTTP_TRAILERS
         || h->state == HTTP_CHUNK_END) {
            uint8_t c = in[used++];

            if (h->state == HTTP_CHUNK_END) {
                /* the crlf after a chunk's data. */
                if (c == '\n') {
                    h->state = HTTP_CHUNK_SIZE;
                    h->line_len = 0;
                }
                continue;
            }

            if (c == '\r') {
                continue;       /* the \n is what ends a line */
            }
            if (c == '\n') {
                got_line(h);
                continue;
            }
            if (h->line_len + 1 < HTTP_LINE_MAX) {
                h->line[h->line_len++] = (char)c;
            } else {
                h->line_too_long = true;
            }
            continue;
        }

        /*
         * a full output buffer stops this by way of `take` being zero
         * below, since there is no room to take anything into. there
         * was an explicit test for it here as well; break-testing it
         * showed it could not fail on its own, and two tests for one
         * condition is one that eventually gets edited alone
         */
        size_t room = max - produced;
        size_t left = len - used;
        size_t take = left < room ? left : room;

        if (h->state == HTTP_BODY || h->state == HTTP_CHUNK_DATA) {
            if ((uint64_t)take > h->length) {
                take = (size_t)h->length;
            }
        }
        if (take == 0) {
            break;
        }

        memcpy(out + produced, in + used, take);
        produced += take;
        used     += take;
        h->received += take;

        if (h->state == HTTP_BODY) {
            h->length -= take;
            if (h->length == 0) {
                h->state = HTTP_DONE;
            }
        } else if (h->state == HTTP_CHUNK_DATA) {
            h->length -= take;
            if (h->length == 0) {
                h->state = HTTP_CHUNK_END;
            }
        }
        /* HTTP_EOF just keeps taking until the connection closes */
    }

    *out_len = produced;
    return used;
}

void http_closed(struct http *h)
{
    switch (h->state) {
    case HTTP_EOF:
        /* the declared way for a body with no length to end */
        h->state = HTTP_DONE;
        break;
    case HTTP_DONE:
    case HTTP_BROKEN:
        break;
    default:
        /*
         * closed in the middle of a body whose length was declared, or
         * before the headers finished. what arrived is a fragment, and
         * calling it complete is how half a file gets written over a
         * whole one
         */
        h->state = HTTP_BROKEN;
        break;
    }
}

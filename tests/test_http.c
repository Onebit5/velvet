// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_http.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the http client.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "net/net.h"
#include "net/http.h"
#include "fixtures/http_captured.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* run a whole response through, in pieces of `piece` bytes, and collect the body. */
struct outcome {
    enum http_state state;
    int    status;
    size_t consumed;            /* input bytes the machine actually took */
    size_t body_len;
    char   body[4096];
    char   content_type[HTTP_VALUE_MAX];
    char   location[HTTP_VALUE_MAX];
};

static void run(const char *response, size_t len, size_t piece,
                bool then_close, struct outcome *o)
{
    struct http h;
    http_start(&h);

    memset(o, 0, sizeof *o);

    size_t at = 0;
    while (at < len) {
        size_t n = (piece == 0) ? len - at : piece;
        if (at + n > len) {
            n = len - at;
        }

        /*
         * the parser may consume less than it is given when the output
         * fills, so this loops until the piece is used up, which is
         * exactly what a caller with a small buffer has to do
         */
        size_t used_total = 0;
        while (used_total < n) {
            uint8_t out[64];
            size_t produced = 0;
            size_t used = http_feed(&h, (const uint8_t *)response + at + used_total,
                                    n - used_total, out, sizeof out, &produced);

            if (produced > 0 && o->body_len + produced < sizeof o->body) {
                memcpy(o->body + o->body_len, out, produced);
                o->body_len += produced;
            }
            if (used == 0) {
                break;      /* done, or broken */
            }
            used_total += used;
            o->consumed += used;
        }
        at += n;
        if (h.state == HTTP_DONE || h.state == HTTP_BROKEN) {
            break;
        }
    }

    if (then_close) {
        http_closed(&h);
    }
    o->state  = h.state;
    o->status = h.status;
    memcpy(o->content_type, h.content_type, sizeof o->content_type);
    memcpy(o->location, h.location, sizeof o->location);
}

/*
 * the whole point: drive it split at every offset and every piece size,
 * and check nothing changes
 */
static void every_split(const char *what, const char *response,
                        bool then_close)
{
    size_t len = strlen(response);
    struct outcome whole;
    run(response, len, 0, then_close, &whole);

    for (size_t piece = 1; piece <= len; piece++) {
        struct outcome part;
        run(response, len, piece, then_close, &part);

        if (part.state != whole.state || part.status != whole.status
         || part.body_len != whole.body_len
         || memcmp(part.body, whole.body, whole.body_len) != 0
         || strcmp(part.content_type, whole.content_type) != 0) {
            printf("FAIL: %s, arriving in pieces of %zu changes the "
                   "answer (state %d vs %d, %zu body bytes vs %zu)\n",
                   what, piece, (int)part.state, (int)whole.state,
                   part.body_len, whole.body_len);
            failures++;
            return;
        }
    }
}

int main(void)
{
    struct outcome o;
    char host[128], path[256];
    uint16_t port;
    bool unsupported;



    CHECK(http_parse_url("http://example.com/index.html", host, sizeof host,
                         &port, path, sizeof path, &unsupported),
          "an ordinary url parses");
    CHECK(strcmp(host, "example.com") == 0, "with the host");
    CHECK(port == 80, "the default port");
    CHECK(strcmp(path, "/index.html") == 0, "and the path");

    CHECK(http_parse_url("http://example.com", host, sizeof host, &port,
                         path, sizeof path, &unsupported),
          "a url with no path is a url");
    CHECK(strcmp(path, "/") == 0, "and the path is the root");

    CHECK(http_parse_url("example.com/x", host, sizeof host, &port,
                         path, sizeof path, &unsupported),
          "no scheme at all is taken as http, which is what anybody means");
    CHECK(strcmp(host, "example.com") == 0 && strcmp(path, "/x") == 0,
          "with the host and path found anyway");

    CHECK(http_parse_url("http://example.com:8080/y", host, sizeof host,
                         &port, path, sizeof path, &unsupported),
          "a port is taken");
    CHECK(port == 8080, "and used");

    CHECK(!http_parse_url("https://example.com/", host, sizeof host, &port,
                          path, sizeof path, &unsupported),
          "https is refused");
    CHECK(unsupported,
          "and refused *as a scheme this cannot speak*, which is a "
          "different thing from a url that is malformed, there is no "
          "tls here and pretending otherwise would hang");

    CHECK(!http_parse_url("gopher://example.com/", host, sizeof host, &port,
                          path, sizeof path, &unsupported)
          && unsupported, "and so is anything else with a scheme");

    CHECK(!http_parse_url("", host, sizeof host, &port, path, sizeof path,
                          &unsupported), "an empty string is not a url");
    CHECK(!http_parse_url("http://", host, sizeof host, &port, path,
                          sizeof path, &unsupported),
          "and neither is a scheme with no host");
    CHECK(!http_parse_url("http://example.com:0/", host, sizeof host, &port,
                          path, sizeof path, &unsupported),
          "port zero is not a port");
    CHECK(!http_parse_url("http://example.com:99999/", host, sizeof host,
                          &port, path, sizeof path, &unsupported),
          "and neither is one that does not fit");



    char req[512];
    size_t n = http_get(req, sizeof req, "example.com", "/index.html");
    CHECK(n > 0, "a request is built");
    CHECK(strstr(req, "GET /index.html HTTP/1.0\r\n") == req,
          "asking for the path, over http/1.0, 1.1 makes keep-alive the "
          "default and a client reading until close would wait forever");
    CHECK(strstr(req, "\r\nHost: example.com\r\n") != NULL,
          "with a Host header, without which a server sharing an address "
          "between sites cannot know which was meant");
    CHECK(strstr(req, "\r\nConnection: close\r\n") != NULL,
          "and asking for the connection to end");
    CHECK(strstr(req, "\r\n\r\n") != NULL, "ending with a blank line");

    CHECK(http_get(req, 20, "example.com", "/x") == 0,
          "a request that will not fit is refused rather than cut short");



    static const char simple[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 12\r\n"
        "\r\n"
        "hello, world";

    run(simple, strlen(simple), 0, false, &o);
    CHECK(o.state == HTTP_DONE, "a response with a length completes");
    CHECK(o.status == 200, "with its status");
    CHECK(o.body_len == 12 && memcmp(o.body, "hello, world", 12) == 0,
          "and its body");
    CHECK(strcmp(o.content_type, "text/plain") == 0, "and the content type");

    every_split("a body of declared length", simple, false);

    /*
     * the one that is not optional: ask a modern server for anything and
     * this is very often what comes back, because a server generating a
     * page as it sends does not know the length when the headers go
     */

    static const char chunked[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "7\r\n, world\r\n"
        "0\r\n"
        "\r\n";

    run(chunked, strlen(chunked), 0, false, &o);
    CHECK(o.state == HTTP_DONE, "a chunked response completes");
    CHECK(o.body_len == 12 && memcmp(o.body, "hello, world", 12) == 0,
          "with the chunks joined and the lengths taken out");

    every_split("a chunked body", chunked, false);

    /* a chunk size in capitals, with an extension after it */
    static const char chunked_odd[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "A;name=value\r\n0123456789\r\n"
        "0\r\n\r\n";

    run(chunked_odd, strlen(chunked_odd), 0, false, &o);
    CHECK(o.state == HTTP_DONE, "a chunk size in hex capitals is a size");
    CHECK(o.body_len == 10 && memcmp(o.body, "0123456789", 10) == 0,
          "and an extension after it is ignored rather than read");

    every_split("a chunked body with extensions", chunked_odd, false);

    /* trailers after the last chunk */
    static const char chunked_trailer[] =
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "3\r\nabc\r\n"
        "0\r\n"
        "X-Checksum: 1234\r\n"
        "\r\n";

    run(chunked_trailer, strlen(chunked_trailer), 0, false, &o);
    CHECK(o.state == HTTP_DONE, "trailers after the last chunk end it");
    CHECK(o.body_len == 3, "and are not mistaken for body");
    CHECK(o.consumed == strlen(chunked_trailer),
          "and are read through rather than left in the stream, a "
          "response that finished at the zero chunk looks identical from "
          "the outside until you ask how much of it was consumed");

    every_split("a chunked body with trailers", chunked_trailer, false);



    static const char until_close[] =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "no length was given";

    run(until_close, strlen(until_close), 0, true, &o);
    CHECK(o.state == HTTP_DONE,
          "a body with no declared length ends when the connection does, "
          "which is http/1.0's original answer and still legal");
    CHECK(o.body_len == 19, "and everything before that is the body");

    every_split("a body ended by the close", until_close, true);

    /* and the same response *without* the close is not finished */
    run(until_close, strlen(until_close), 0, false, &o);
    CHECK(o.state != HTTP_DONE,
          "and until the connection closes it is not complete");



    static const char cut_short[] =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 100\r\n"
        "\r\n"
        "only this much";

    run(cut_short, strlen(cut_short), 0, true, &o);
    CHECK(o.state == HTTP_BROKEN,
          "a connection closing part way through a declared length is a "
          "truncation, not an ending, calling it complete is how half a "
          "file gets written over a whole one");

    /* closed before the headers even finished */
    run("HTTP/1.1 200 OK\r\nContent-Len", 28, 0, true, &o);
    CHECK(o.state == HTTP_BROKEN, "and so is one closing mid-header");



    static const char not_found[] =
        "HTTP/1.1 404 Not Found\r\nContent-Length: 3\r\n\r\nno!";
    run(not_found, strlen(not_found), 0, false, &o);
    CHECK(o.state == HTTP_DONE && o.status == 404,
          "a 404 is a complete response that happens to say no");

    static const char redirect[] =
        "HTTP/1.1 301 Moved Permanently\r\n"
        "Location: https://example.com/\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    run(redirect, strlen(redirect), 0, false, &o);
    CHECK(o.status == 301, "a redirect is read");
    CHECK(strcmp(o.location, "https://example.com/") == 0,
          "and where it points is kept, following one silently is how a "
          "fetch ends up with something other than what was asked for");



    run("not http at all\r\n\r\n", 19, 0, true, &o);
    CHECK(o.state == HTTP_BROKEN, "something that is not a response is not");

    run("HTTP/1.1 abc OK\r\n\r\n", 19, 0, true, &o);
    CHECK(o.state == HTTP_BROKEN, "and neither is one with no status number");

    run("HTTP/1.1\r\n\r\n", 12, 0, true, &o);
    CHECK(o.state == HTTP_BROKEN, "or one with no status at all");

    /* a header with no colon is dropped rather than being fatal */
    static const char stray[] =
        "HTTP/1.1 200 OK\r\n"
        "this line has no colon\r\n"
        "Content-Length: 2\r\n"
        "\r\nhi";
    run(stray, strlen(stray), 0, false, &o);
    CHECK(o.state == HTTP_DONE && o.body_len == 2,
          "a stray header line is ignored rather than ruining the body, "
          "one bad line does not make the rest unreadable");

    every_split("a response with a stray header", stray, false);

    /* a chunk size that is not a number */
    static const char bad_chunk[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "zz\r\nabc\r\n0\r\n\r\n";
    run(bad_chunk, strlen(bad_chunk), 0, true, &o);
    CHECK(o.state == HTTP_BROKEN, "a chunk size that is not one is refused");

    /* an absurd content-length */
    /*
     * 18446744073709551621 is 2^64 + 5, so an unchecked accumulator
     * wraps it round to *five*, which then reads five bytes and calls
     * the response complete. a length that merely wraps to something
     * large is caught by the close instead, which is why the first
     * version of this test passed with the check removed
     */
    static const char silly_length[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 18446744073709551621\r\n"
        "\r\nhello";
    run(silly_length, strlen(silly_length), 0, true, &o);
    CHECK(o.state == HTTP_BROKEN,
          "a length nothing could hold is refused rather than wrapped "
          "round into a small one that looks perfectly reasonable");

    /* a header line longer than the buffer */
    {
        char huge[HTTP_LINE_MAX * 2];
        size_t w = 0;
        const char *head = "HTTP/1.1 200 OK\r\nX-Long: ";
        memcpy(huge + w, head, strlen(head)); w += strlen(head);
        for (size_t i = 0; i < HTTP_LINE_MAX + 100; i++) {
            huge[w++] = 'a';
        }
        const char *tail = "\r\nContent-Length: 2\r\n\r\nhi";
        memcpy(huge + w, tail, strlen(tail)); w += strlen(tail);

        run(huge, w, 0, false, &o);
        CHECK(o.state == HTTP_DONE && o.body_len == 2,
              "a header longer than the buffer is dropped whole rather "
              "than acted on in part, half a header is one nobody sent");
    }

    /* and one whose surviving prefix would be *believed*. */
    {
        char huge[HTTP_LINE_MAX * 2];
        size_t w = 0;
        const char *head = "HTTP/1.1 200 OK\r\nContent-Length: 99";
        memcpy(huge + w, head, strlen(head)); w += strlen(head);
        for (size_t i = 0; i < HTTP_LINE_MAX + 100; i++) {
            huge[w++] = 'x';
        }
        const char *tail = "\r\n\r\nhello";
        memcpy(huge + w, tail, strlen(tail)); w += strlen(tail);

        run(huge, w, 0, true, &o);
        CHECK(o.state == HTTP_DONE,
              "a header cut off by the buffer is dropped, so the body "
              "ends with the connection rather than at a length nobody "
              "actually sent");
        CHECK(o.body_len == 5, "and all of it is kept");
    }

    /* a status line longer than the buffer is fatal, unlike a header */
    {
        char huge[HTTP_LINE_MAX * 2];
        size_t w = 0;
        for (size_t i = 0; i < HTTP_LINE_MAX + 100; i++) {
            huge[w++] = 'A';
        }
        const char *tail = "\r\n\r\n";
        memcpy(huge + w, tail, strlen(tail)); w += strlen(tail);
        run(huge, w, 0, true, &o);
        CHECK(o.state == HTTP_BROKEN,
              "but a status line that long leaves nothing to go on");
    }



    static const char lf_only[] =
        "HTTP/1.1 200 OK\nContent-Length: 5\n\nhello";
    run(lf_only, strlen(lf_only), 0, false, &o);
    CHECK(o.state == HTTP_DONE && o.body_len == 5,
          "a server using bare newlines is understood, the \\n is what "
          "ends a line here and the \\r is merely allowed");

    every_split("a response with bare newlines", lf_only, false);

    /*
     * every fixture above is laid out by hand from the rfc, which is
     * weaker than it looks: it removes one author from the comparison
     * and not the other, since its *reading* of the spec is still on
     * both sides of it.
     *
     * this one removes both. the bytes came off the wire from
     * example.com, served by cloudflare, and the body they should
     * decode to is what curl made of them. it is chunked, the framing
     * `python3 -m http.server` never sends, so no boot test had ever
     * exercised it against a real server either
     */
    {
        struct http h;
        http_start(&h);

        static uint8_t body[4096];
        size_t total = 0;
        size_t at = 0;

        while (at < sizeof http_captured_raw) {
            uint8_t out[64];
            size_t produced = 0;
            size_t used = http_feed(&h, http_captured_raw + at,
                                    sizeof http_captured_raw - at,
                                    out, sizeof out, &produced);
            if (produced > 0 && total + produced <= sizeof body) {
                memcpy(body + total, out, produced);
                total += produced;
            }
            if (used == 0) {
                break;
            }
            at += used;
        }

        CHECK(h.state == HTTP_DONE, "a real chunked response completes");
        CHECK(h.status == 200, "with its status");
        CHECK(total == sizeof http_captured_body,
              "and exactly as many body bytes as curl got out of the same "
              "response, one byte either way means the chunk framing is "
              "being copied through or eaten");
        CHECK(total == sizeof http_captured_body
              && memcmp(body, http_captured_body, total) == 0,
              "and the same bytes, which is the only comparison in this "
              "file where neither side is the test's");

        /* and the property that matters, on bytes nobody here chose */
        for (size_t piece = 1; piece <= 64; piece++) {
            struct http g;
            http_start(&g);
            size_t got_total = 0, k = 0;
            bool bad = false;

            while (k < sizeof http_captured_raw) {
                size_t n = piece;
                if (k + n > sizeof http_captured_raw) {
                    n = sizeof http_captured_raw - k;
                }
                size_t inner = 0;
                while (inner < n) {
                    uint8_t out[16];
                    size_t produced = 0;
                    size_t used = http_feed(&g, http_captured_raw + k + inner,
                                            n - inner, out, sizeof out,
                                            &produced);
                    if (produced > 0) {
                        if (got_total + produced > sizeof http_captured_body
                         || memcmp(http_captured_body + got_total, out,
                                   produced) != 0) {
                            bad = true;
                        }
                        got_total += produced;
                    }
                    if (used == 0) {
                        break;
                    }
                    inner += used;
                }
                k += n;
                if (g.state == HTTP_DONE || g.state == HTTP_BROKEN) {
                    break;
                }
            }
            if (bad || got_total != sizeof http_captured_body
             || g.state != HTTP_DONE) {
                printf("FAIL: a real chunked response arriving in pieces "
                       "of %zu decodes differently\n", piece);
                failures++;
                break;
            }
        }
    }

    if (failures == 0) printf("all good\n");
    return failures;
}

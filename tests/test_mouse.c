// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_mouse.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the mouse.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "drivers/mouse.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* feed a run of bytes, keeping the last event that came out */
static int feed(struct mouse_decoder *d, const uint8_t *bytes, size_t n,
                struct mouse_event *last)
{
    int events = 0;
    for (size_t i = 0; i < n; i++) {
        struct mouse_event ev;
        if (mouse_decode(d, bytes[i], &ev)) {
            events++;
            if (last) {
                *last = ev;
            }
        }
    }
    return events;
}

int main(void)
{
    struct mouse_decoder d;
    struct mouse_event ev;



    mouse_decoder_init(&d, 3);
    /* flags with the always-set bit, then +5 across and +3 up */
    uint8_t still[] = { 0x08, 5, 3 };
    CHECK(feed(&d, still, 3, &ev) == 1, "three bytes are one packet");
    CHECK(ev.dx == 5, "with the movement across");
    CHECK(ev.dy == -3,
          "and the movement up, already flipped, the mouse counts up "
          "and screens count down, and doing it here means nothing above "
          "has to remember");
    CHECK(ev.buttons == 0, "and no buttons");

    /* two bytes is not a packet yet */
    CHECK(feed(&d, still, 2, NULL) == 0, "two bytes are not");



    mouse_decoder_init(&d, 3);
    uint8_t left_down[] = { 0x08 | 0x10, 0xfb, 0 };     /* x sign set, -5 */
    feed(&d, left_down, 3, &ev);
    CHECK(ev.dx == -5,
          "a negative movement is a sign bit in one byte and the value in "
          "another, which is the whole of why this needs decoding");

    uint8_t up[] = { 0x08 | 0x20, 0, 0xfb };            /* y sign set, -5 */
    feed(&d, up, 3, &ev);
    CHECK(ev.dy == 5, "and downwards on the mouse is upwards on the screen");

    /* an overflow means it moved further than a byte can say. */
    uint8_t overflowed[] = { 0x08 | 0x40 | 0x01, 200, 200 };
    feed(&d, overflowed, 3, &ev);
    CHECK(ev.dx == 0 && ev.dy == 0,
          "an overflowed packet reports no movement rather than a wrong one");
    CHECK(ev.buttons == MOUSE_LEFT, "but its buttons are still read");

    /* a packet says what is held *now*. */

    mouse_decoder_init(&d, 3);

    uint8_t press_left[] = { 0x08 | MOUSE_LEFT, 0, 0 };
    feed(&d, press_left, 3, &ev);
    CHECK(ev.buttons == MOUSE_LEFT, "the left button is held");
    CHECK(ev.pressed == MOUSE_LEFT, "and it went down in this packet");
    CHECK(ev.released == 0, "and nothing came up");

    /* held, but not pressed again */
    feed(&d, press_left, 3, &ev);
    CHECK(ev.buttons == MOUSE_LEFT, "it is still held");
    CHECK(ev.pressed == 0,
          "and is not pressed again, holding a button is not pressing it "
          "sixty times a second, however many packets say it is down");

    uint8_t press_both[] = { 0x08 | MOUSE_LEFT | MOUSE_RIGHT, 0, 0 };
    feed(&d, press_both, 3, &ev);
    CHECK(ev.pressed == MOUSE_RIGHT, "a second button going down is noticed");
    CHECK(ev.released == 0, "without the first coming up");

    uint8_t none[] = { 0x08, 0, 0 };
    feed(&d, none, 3, &ev);
    CHECK(ev.released == (MOUSE_LEFT | MOUSE_RIGHT),
          "and both coming up at once is both");
    CHECK(ev.buttons == 0, "with nothing held after");

    /* the 8042 hands over one byte at a time and nothing says where a packet starts. */

    mouse_decoder_init(&d, 3);
    uint64_t before = mouse_resyncs();

    /* a stray byte with the marker clear, arriving where a packet should start. */
    uint8_t stray[] = { 0x00 };
    CHECK(feed(&d, stray, 1, NULL) == 0, "a byte that cannot start a packet");
    CHECK(mouse_resyncs() > before, "is dropped, and counted");

    /* and the packet after it still reads correctly */
    uint8_t after[] = { 0x08, 7, 0 };
    CHECK(feed(&d, after, 3, &ev) == 1, "the next packet is still a packet");
    CHECK(ev.dx == 7, "and reads correctly, having not been shifted along");

    /* a whole run of rubbish, then a real packet */
    uint8_t rubbish[] = { 0x00, 0x01, 0x02, 0x00, 0x07 };
    feed(&d, rubbish, sizeof rubbish, NULL);
    CHECK(feed(&d, after, 3, &ev) == 1, "a run of rubbish is skipped whole");
    CHECK(ev.dx == 7, "and what follows is read correctly");

    /* the nasty one: a *truncated* packet. */
    mouse_decoder_init(&d, 3);
    uint8_t half[] = { 0x08, 1 };
    feed(&d, half, 2, NULL);
    uint8_t whole[] = { 0x08, 9, 0 };
    int got = feed(&d, whole, 3, &ev);
    CHECK(got == 1,
          "a truncated packet costs exactly one wrong packet and then it "
          "is back in step");



    mouse_decoder_init(&d, 4);
    uint8_t wheel_up[] = { 0x08, 0, 0, 0x0f };      /* -1 in four bits */
    CHECK(feed(&d, wheel_up, 4, &ev) == 1, "a four byte packet is one packet");
    CHECK(ev.wheel == 1, "and the wheel is read, counting the same way up");

    uint8_t wheel_down[] = { 0x08, 0, 0, 0x01 };
    feed(&d, wheel_down, 4, &ev);
    CHECK(ev.wheel == -1, "and the same way down");

    uint8_t wheel_none[] = { 0x08, 0, 0, 0x00 };
    feed(&d, wheel_none, 4, &ev);
    CHECK(ev.wheel == 0, "and is zero when it did not turn");

    /* three bytes of a four byte packet is not a packet */
    mouse_decoder_init(&d, 4);
    CHECK(feed(&d, wheel_up, 3, NULL) == 0,
          "three bytes of a four byte packet are not a packet");

    /*
     * a decoder asked for a nonsense size takes three, which is the
     * size every ps/2 mouse has ever had
     */
    mouse_decoder_init(&d, 7);
    CHECK(feed(&d, still, 3, &ev) == 1,
          "and a nonsense packet size falls back to three");

    if (failures == 0) printf("all good\n");
    return failures;
}

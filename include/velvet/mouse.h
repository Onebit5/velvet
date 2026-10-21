// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/mouse.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the ps/2 mouse: the first input here that is not a stream of characters.
 */

#ifndef VELVET_MOUSE_H
#define VELVET_MOUSE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* the ps/2 mouse: the first input here that is not a stream of characters. */

#define MOUSE_LEFT   0x1
#define MOUSE_RIGHT  0x2
#define MOUSE_MIDDLE 0x4

struct mouse_event {
    int  dx, dy;        /* since the last event. y is already flipped:
                         * the mouse counts up, screens count down */
    int  wheel;         /* -1, 0 or 1 on a mouse that has one */
    uint8_t buttons;    /* what is held *now* */
    uint8_t pressed;    /* what went down in this event */
    uint8_t released;   /* and what came up */
};

/* feed it bytes as they arrive. */

struct mouse_decoder {
    uint8_t  packet[4];
    unsigned at;
    unsigned size;      /* 3, or 4 once the wheel has been negotiated */
    uint8_t  buttons;   /* what was held last time, to work out edges */
};

void mouse_decoder_init(struct mouse_decoder *d, unsigned packet_size);
bool mouse_decode(struct mouse_decoder *d, uint8_t byte,
                  struct mouse_event *out);

/* find one, wake it up, and start listening. */
void mouse_init(void);

/* how many packets were thrown away for arriving out of step. */
uint64_t mouse_resyncs(void);

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/mouse.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the ps/2 mouse, and its decoder.
 */

#include <velvet/mouse.h>
#include <velvet/console.h>
#include <velvet/input.h>
#include <asm/io.h>
#include <asm/pic.h>
#include <asm/interrupts.h>
#include <velvet/kprintf.h>
#include <velvet/string.h>

/*
 * a packet is three bytes and the first one is where all the awkwardness
 * lives: three button bits, then a bit that is *always set*, then the
 * sign bits for the two movement bytes that follow.
 *
 * that always-set bit is the only way to tell where a packet starts. the
 * 8042 has no framing, if one byte is dropped or one arrives before
 * the driver is listening, every packet after it is read one byte out of
 * step and the pointer flies off in a straight line. checking the bit
 * and throwing the byte away is the whole of the fix, and it is why the
 * count of discarded bytes is worth reporting: it is the only symptom a
 * desynchronised mouse has.
 */

#define FLAG_ALWAYS_1 0x08
#define FLAG_X_SIGN   0x10
#define FLAG_Y_SIGN   0x20
#define FLAG_X_OVER   0x40
#define FLAG_Y_OVER   0x80

static uint64_t resyncs;

void mouse_decoder_init(struct mouse_decoder *d, unsigned packet_size)
{
    memset(d, 0, sizeof *d);
    d->size = (packet_size == 4) ? 4 : 3;
}

bool mouse_decode(struct mouse_decoder *d, uint8_t byte,
                  struct mouse_event *out)
{
    if (d->at == 0 && !(byte & FLAG_ALWAYS_1)) {
        /* not the start of a packet. */
        resyncs++;
        return false;
    }

    d->packet[d->at++] = byte;
    if (d->at < d->size) {
        return false;
    }
    d->at = 0;

    uint8_t flags = d->packet[0];
    memset(out, 0, sizeof *out);

    /* an overflow means the mouse moved further between reports than a byte can say. */
    if (!(flags & (FLAG_X_OVER | FLAG_Y_OVER))) {
        int dx = d->packet[1];
        int dy = d->packet[2];
        if (flags & FLAG_X_SIGN) {
            dx -= 256;
        }
        if (flags & FLAG_Y_SIGN) {
            dy -= 256;
        }
        out->dx = dx;
        /*
         * the mouse counts up and screens count down, so this is the
         * one place the sign is flipped, doing it here means nothing
         * above ever has to remember to
         */
        out->dy = -dy;
    }

    if (d->size == 4) {
        int8_t w = (int8_t)(d->packet[3] & 0x0f);
        if (w & 0x08) {
            w |= (int8_t)0xf0;      /* four bits, signed */
        }
        out->wheel = -w;            /* and the wheel counts the other way too */
    }

    uint8_t now = flags & (MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE);
    out->buttons = now;
    out->pressed = (uint8_t)(now & ~d->buttons);
    out->released = (uint8_t)(d->buttons & ~now);
    d->buttons = now;

    return true;
}

uint64_t mouse_resyncs(void)
{
    return resyncs;
}



#ifndef VELVET_HOSTED

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_COMMAND 0x64

#define MOUSE_IRQ 12

static bool present;
static bool wheel;
static struct mouse_decoder decoder;

/* where the pointer is, in characters. */
static size_t pointer_col, pointer_row;
static uint8_t held;

/* the 8042 is slow and says so. */
static bool wait_writable(void)
{
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) {
            return true;
        }
    }
    return false;
}

static bool wait_readable(void)
{
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01) {
            return true;
        }
    }
    return false;
}

static void command(uint8_t byte)
{
    wait_writable();
    outb(PS2_COMMAND, byte);
}

/* a byte for the mouse rather than for the keyboard controller. */
static bool to_mouse(uint8_t byte)
{
    command(0xd4);
    if (!wait_writable()) {
        return false;
    }
    outb(PS2_DATA, byte);

    if (!wait_readable()) {
        return false;
    }
    return inb(PS2_DATA) == 0xfa;       /* the mouse acknowledges */
}

static void mouse_irq(void)
{

    /* bit 5 of the status byte says this came from the mouse rather than the keyboard. */
    uint8_t status = inb(PS2_STATUS);
    if (!(status & 0x20)) {
        return;
    }

    uint8_t byte = inb(PS2_DATA);

    struct mouse_event ev;
    if (!mouse_decode(&decoder, byte, &ev)) {
        return;
    }

    size_t cols = 0, rows = 0;
    console_size(&cols, &rows, NULL, NULL);
    if (cols == 0 || rows == 0) {
        return;
    }

    /*
     * the mouse reports in its own counts and this is a text console,
     * so the movement is divided down. a pointer that could sit between
     * two cells would be pointing at nothing
     */
    static int fine_x, fine_y;
    fine_x += ev.dx;
    fine_y += ev.dy;

    int moved_x = fine_x / 8;
    int moved_y = fine_y / 16;
    fine_x -= moved_x * 8;
    fine_y -= moved_y * 16;

    long col = (long)pointer_col + moved_x;
    long row = (long)pointer_row + moved_y;
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    if (col >= (long)cols) col = (long)cols - 1;
    if (row >= (long)rows) row = (long)rows - 1;

    pointer_col = (size_t)col;
    pointer_row = (size_t)row;
    held = ev.buttons;

    console_pointer(pointer_col, pointer_row, ev.buttons, ev.pressed,
                    ev.released);

    /* the middle button types the selection back. */
    if (ev.pressed & MOUSE_MIDDLE) {
        static char pasted[512];
        size_t n = console_selection(pasted, sizeof pasted);
        for (size_t i = 0; i < n; i++) {
            /*
             * a newline in the middle of a paste would submit the line
             * and hand the rest to whatever ran. one line is pasted and
             * the rest is dropped, which is what every terminal does
             * and for the same reason
             */
            if (pasted[i] == '\n') {
                break;
            }
            input_push((unsigned char)pasted[i]);
        }
    }

    /*
     * the wheel looks back up the console, the same as shift+pageup,
     * a wheel is a scrollback control on every terminal there has ever
     * been and it would be strange for it not to be one here
     */
    if (ev.wheel != 0) {
        console_scroll_back(ev.wheel * 3);
    }
}

void mouse_init(void)
{
    /* the auxiliary port, which is off until asked */
    command(0xa8);

    /* and the controller has to be told to raise an interrupt for it. */
    command(0x20);
    if (!wait_readable()) {
        kprintf("mouse      : the 8042 never answered. no pointer\n");
        return;
    }
    uint8_t config = inb(PS2_DATA);
    config |= 0x02;         /* interrupt on the auxiliary port */
    config &= (uint8_t)~0x20;
    command(0x60);
    wait_writable();
    outb(PS2_DATA, config);

    /* defaults, and *not* reporting yet. */
    if (!to_mouse(0xf6)) {
        kprintf("mouse      : nothing answered on the auxiliary port\n");
        return;
    }
    to_mouse(0xf5);     /* reporting off while the kernel asks it questions */

    /*
     * the wheel handshake, which can only be historical: set the sample
     * rate to 200, then 100, then 80, and ask who you are. a mouse that
     * understands answers 3 instead of 0 and starts sending a fourth
     * byte. nobody would design this; it was the only way to add a byte
     * to a protocol that had no version number.
     *
     * with reporting *on*, movement packets interleave with the
     * acknowledgements and the whole conversation goes out of step,
     * which leaves bytes in the controller that the keyboard handler
     * then reads as scancodes. that is worth turning off for
     */
    unsigned size = 3;
    if (to_mouse(0xf3) && to_mouse(200)
        && to_mouse(0xf3) && to_mouse(100)
        && to_mouse(0xf3) && to_mouse(80)
        && to_mouse(0xf2) && wait_readable()) {
        if (inb(PS2_DATA) == 3) {
            wheel = true;
            size = 4;
        }
    }

    /* whatever is left of that conversation is nobody's data. */
    while (inb(PS2_STATUS) & 0x01) {
        inb(PS2_DATA);
    }

    if (!to_mouse(0xf4)) {      /* and now it may talk */
        kprintf("mouse      : it would not start reporting\n");
        return;
    }

    mouse_decoder_init(&decoder, size);
    present = true;

    size_t cols = 0, rows = 0;
    console_size(&cols, &rows, NULL, NULL);
    pointer_col = cols / 2;
    pointer_row = rows / 2;

    irq_install(MOUSE_IRQ, mouse_irq);

    kprintf("mouse      : ps/2%s, %u-byte packets\n",
            wheel ? " with a wheel" : "", size);
}

#endif /* VELVET_HOSTED */

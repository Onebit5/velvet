// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/uart.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * com1, a 16550 uart (or whatever qemu pretends is one).
 */

#include "drivers/serial.h"
#include "drivers/input.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/interrupts.h"

/*
 * com1, a 16550 uart (or whatever qemu pretends is one).
 *
 * split out of drivers/serial.c, and the split is the point:
 * everything here is `outb` and a port number, and everything left
 * behind is a terminal escape sequence. the chip is not the x86 part,
 * an 8250 wired to memory rather than to a port space would need this
 * file rewritten and none of the other one.
 */

#define COM1     0x3f8
#define COM1_IRQ 4

static bool serial_ok = false;

bool serial_init(void)
{
    outb(COM1 + 1, 0x00);   /* no uart interrupts, the kernel polls like cavemen for now */
    outb(COM1 + 3, 0x80);   /* dlab on so the divisor registers are visible */
    outb(COM1 + 0, 0x01);   /* divisor 1 -> 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);   /* 8n1, dlab back off */
    outb(COM1 + 2, 0xc7);   /* enable + clear fifos */
    outb(COM1 + 4, 0x1e);   /* loopback mode, to check the chip actually works */

    /*
     * FIXME: this reads the byte back before it can possibly have
     * arrived. in loopback the transmitter drives the receiver through
     * the same shift registers, so the character takes one character time
     * to come round: at 115200 baud and 8n1 that is about 87
     * microseconds, and the two port writes above are over in a fraction
     * of one. qemu hands it back instantly, which is why this has always
     * looked like it works, and on a real 16550 the read returns whatever
     * was in the register beforehand, the test fails, and the kernel
     * decides the port is dead and stays quiet for the rest of the boot.
     * wait for the data-ready bit first, with a bound.
     */
    outb(COM1 + 0, 0xae);   /* random test byte */
    if (inb(COM1 + 0) != 0xae) {
        return false;       /* dead or missing uart. sad but not fatal */
    }

    outb(COM1 + 4, 0x0f);   /* normal operation, rts/dtr set */
    serial_ok = true;
    return true;
}

static bool transmit_empty(void)
{
    return inb(COM1 + 5) & 0x20;
}

void serial_putchar(char c)
{
    if (!serial_ok) {
        return;
    }
    if (c == '\n') {
        serial_putchar('\r');   /* terminals want crlf */
    }
    while (!transmit_empty()) {
        /* spin. its fine, its 115200 baud */
    }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s)
{
    while (*s) {
        serial_putchar(*s++);
    }
}



static void serial_irq(void)
{
    /* drain the fifo, the kernel may have been handed several bytes at once */
    while (inb(COM1 + 5) & 1) {
        serial_feed(inb(COM1));
    }
}

void serial_input_init(void)
{
    if (!serial_ok) {
        return;
    }
    outb(COM1 + 1, 0x01);   /* interrupt when a byte arrives */
    irq_install(COM1_IRQ, serial_irq);
}

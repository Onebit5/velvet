// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/pic.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the 8259 pic pair.
 */

#include <asm/pic.h>
#include <asm/io.h>

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xa0
#define PIC2_DATA 0xa1

#define PIC_EOI   0x20
#define ICW1_INIT 0x11      /* init, cascade mode, expect icw4 */
#define ICW4_8086 0x01

void pic_init(void)
{
    /*
     * the bios leaves irqs mapped over vectors 8-15, right on top of the
     * cpu exceptions, which is a 40 year old design accident the kernel now fix
     * at every single boot. shift everything to 32+
     */
    outb(PIC1_CMD, ICW1_INIT);
    io_wait();
    outb(PIC2_CMD, ICW1_INIT);
    io_wait();
    outb(PIC1_DATA, PIC_IRQ_BASE);       /* master: vectors 32-39 */
    io_wait();
    outb(PIC2_DATA, PIC_IRQ_BASE + 8);   /* slave: vectors 40-47 */
    io_wait();
    outb(PIC1_DATA, 4);                  /* slave hangs off line 2 */
    io_wait();
    outb(PIC2_DATA, 2);
    io_wait();
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /*
     * mask everything except the cascade line. every driver unmasks its
     * own irq when its actually ready to hear from it
     */
    outb(PIC1_DATA, 0xfb);
    outb(PIC2_DATA, 0xff);
}

void pic_mask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (1 << (irq & 7)));
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

void pic_send_eoi(uint8_t irq)
{
    if (irq >= 8) {
        outb(PIC2_CMD, PIC_EOI);
    }
    outb(PIC1_CMD, PIC_EOI);
}

static uint8_t pic_read_isr(uint16_t cmd_port)
{
    outb(cmd_port, 0x0b);   /* ocw3: read in-service register */
    return inb(cmd_port);
}

bool pic_is_spurious(uint8_t irq)
{
    /*
     * electrical noise can make the pic announce irq 7 (or 15 on the
     * slave) that no device actually raised. the tell: its not marked
     * in-service. no eoi for ghosts, except the master still saw the
     * cascade for a fake 15
     */
    if (irq == 7 && !(pic_read_isr(PIC1_CMD) & 0x80)) {
        return true;
    }
    if (irq == 15 && !(pic_read_isr(PIC2_CMD) & 0x80)) {
        outb(PIC1_CMD, PIC_EOI);
        return true;
    }
    return false;
}

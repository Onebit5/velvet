// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/pit.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * Imable interval timer.
 */

#include "drivers/pit.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/io.h"
#include "arch/cpu.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/interrupts.h"
#include "sched/sched.h"

#define PIT_CH0  0x40
#define PIT_CH2  0x42
#define PIT_CMD  0x43

/* channel 2's gate and its output, on the keyboard controller of all places. */
#define PORT_61       0x61
#define P61_GATE      0x01
#define P61_SPEAKER   0x02
#define P61_CH2_OUT   0x20
#define PIT_IRQ  0

/*
 * the crystal runs at 1.193182 MHz because of a 1981 decision to reuse
 * the ntsc colorburst divider. the kernel has all been living with it since
 */
#define PIT_BASE_HZ 1193182

static volatile uint64_t ticks;

void pit_tick(void)
{
    /*
     * every core has its own timer and every one of them arrives here,
     * but there is only one clock. if all four counted, an hour would
     * pass in fifteen minutes and every sleep in the system would end
     * early, so the boot core keeps time and the rest just schedule
     */
    if (smp_this_cpu() == 0) {
        ticks++;
    }

    /*
     * the scheduler, though, is every core's business: this is the
     * interrupt that tells whichever thread is here that its turn is up
     */
    sched_tick();
}

static void pit_irq(void)
{
    pit_tick();
}

void pit_stop(void)
{
    /* mode 0, and no reload: it counts down once and stops. */
    outb(PIT_CMD, 0x30);
    outb(PIT_CH0, 0);
    outb(PIT_CH0, 0);
    pic_mask(PIT_IRQ);
}

void pit_init(void)
{
    uint16_t divisor = PIT_BASE_HZ / PIT_HZ;

    /* channel 0, lobyte+hibyte access, mode 2 (rate generator), binary */
    outb(PIT_CMD, 0x34);
    outb(PIT_CH0, divisor & 0xff);
    outb(PIT_CH0, divisor >> 8);

    irq_install(PIT_IRQ, pit_irq);
}

uint64_t pit_ticks(void)
{
    return ticks;
}

uint64_t pit_uptime_ms(void)
{
    return ticks * (1000 / PIT_HZ);
}

void pit_poll_wait(uint64_t ms)
{
    if (ms > 50) {
        ms = 50;        /* 16 bits at 1.193 MHz runs out just past 54 */
    }
    uint64_t count = (PIT_BASE_HZ * ms) / 1000;
    if (count == 0 || count > 0xffff) {
        count = 0xffff;
    }

    uint8_t saved = inb(PORT_61);

    /* gate open, speaker off. nobody wants to calibrate audibly */
    outb(PORT_61, (uint8_t)((saved & ~P61_SPEAKER) | P61_GATE));

    /*
     * channel 2, lobyte then hibyte, mode 0: the output goes low when
     * the count is loaded and high again when it reaches zero
     */
    outb(PIT_CMD, 0xb0);
    outb(PIT_CH2, (uint8_t)(count & 0xff));
    outb(PIT_CH2, (uint8_t)(count >> 8));

    /* bounded, because this runs before anything can report a problem. */
    uint64_t spins = 200000000;
    while (!(inb(PORT_61) & P61_CH2_OUT) && spins-- > 0) {
        cpu_relax();
    }

    outb(PORT_61, saved);
}

void pit_busy_wait(uint64_t ms)
{
    uint64_t until = ticks + (ms / (1000 / PIT_HZ)) + 1;
    while (ticks < until) {
        cpu_relax();
    }
}

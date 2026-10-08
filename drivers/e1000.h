// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/e1000.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the intel 82540em, which is what qemu means by `-device e1000`.
 */

#ifndef DRIVERS_E1000_H
#define DRIVERS_E1000_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "net/net.h"

/* the intel 82540em, which is what qemu means by `-device e1000`. */

/*
 * what qemu presents. 0x100e is the 82540em; the others are the same
 * driver as far as anything here is concerned, and are listed because a
 * machine that says "no card" when there is one is worse than a machine
 * that tries
 */
#define E1000_VENDOR 0x8086

bool e1000_init(void);
bool e1000_present(void);

/* this card's own address, out of its receive-address registers */
const struct mac *e1000_mac(void);

/* put a frame on the wire. */
bool e1000_send(const void *frame, size_t len);

/* take one off. returns 0 when there is nothing waiting. */
size_t e1000_receive(void *out, size_t max);

/*
 * a thread looked at this card twenty times a second, which
 * is late by up to fifty milliseconds and burns a wakeup whether or not
 * anything arrived.
 *
 * the card can say so instead. what an interrupt handler is *allowed to
 * do* is the whole of the design here, and the answer is: read the cause
 * register, and wake somebody. it does not parse a frame, take the
 * interface's lock, or answer an arp, all of that happens on an
 * ordinary thread that can block, print and take locks in any order it
 * likes. the poll loop did not go away; it stopped guessing when to run.
 *
 * "wake a thread" is only legal because of something the scheduler
 * already did: every one of the seventeen places that takes sched_lock
 * takes it with interrupts off. so this interrupt cannot land on a core
 * that is already holding the lock the wake needs, which is the deadlock
 * this design would otherwise have. it is not a new rule, but it is the
 * rule that decides what the handler may do, and it was worth checking
 * before relying on it rather than after.
 *
 * this is also the first pci device in this kernel to use an interrupt
 * at all. the keyboard, the mouse and the timer are isa parts on lines
 * that have not moved since 1981; a pci interrupt is level-triggered,
 * shared with whatever else the firmware put on that line, and switched
 * off by a bit in the command register that nothing here had ever had
 * to clear.
 */

/* called from the interrupt, so it may do almost nothing: wake a thread and return. */
void e1000_on_arrival(void (*fn)(void));

/* whether the card is actually delivering them. */
bool e1000_interrupts_working(void);

/* the line the firmware handed it, or 0 if it got none. */
uint8_t e1000_irq_line(void);

/* what has gone past, for `ifconfig`. */
struct e1000_stats {
    uint64_t sent, received;
    uint64_t send_dropped, receive_dropped;

    /*
     * how many times the card raised one, and how many of those turned
     * out to belong to somebody else on a shared line. the second number
     * being large is a sign the line is busy rather than that anything
     * is wrong
     */
    uint64_t interrupts, not_ours;
};
void e1000_get_stats(struct e1000_stats *out);

const char *e1000_model(void);

#endif

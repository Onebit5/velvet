// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/e1000.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the intel 82540em, which is what qemu means by `-device e1000`.
 */

#ifndef VELVET_E1000_H
#define VELVET_E1000_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <velvet/net.h>

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

const char *e1000_model(void);

#endif

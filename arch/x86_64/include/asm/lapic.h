// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/lapic.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the local apic: one per cpu, and the reason there can be more than one
 * cpu at all.
 */

#ifndef ARCH_X86_64_ASM_LAPIC_H
#define ARCH_X86_64_ASM_LAPIC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * the local apic: one per cpu, and the reason there can be more than
 * one cpu at all. the 8259 is a single chip shared by the machine; this
 * is a piece of the processor, which is why every core gets its own and
 * why an smp kernel cannot be built on the old one.
 *
 * on its own this is lateral, the same interrupts arriving by a
 * better road, and that is worth saying plainly. what it buys is
 * everything after it.
 */

/*
 * the vector the timer arrives on. above the 8259's old range so the
 * two can coexist while the kernel is still deciding which to trust
 */
#define LAPIC_TIMER_VECTOR   0x40
#define LAPIC_SPURIOUS_VECTOR 0xff

bool lapic_init(uint64_t phys_address);
bool lapic_available(void);

/*
 * end of interrupt. every apic interrupt is acknowledged here rather
 * than at the pic, and forgetting is how you get exactly one of each
 */
void lapic_eoi(void);

uint32_t lapic_id(void);

/*
 * wake another core.
 *
 * a processor that has never run holds itself in reset until its own
 * local apic tells it otherwise, and the only thing that can send that
 * message is another core's local apic. so starting a cpu is not a
 * function call, it is an interrupt, addressed to a piece of hardware
 * inside a processor that is not yet executing anything.
 *
 * the sequence is fixed by the manual and is not negotiable: assert
 * INIT, wait, then send STARTUP twice. the vector is the *page* the
 * core begins executing at, in real mode, below one megabyte, so
 * vector 8 means it wakes up at 0x8000 with no idea what year it is
 */
/*
 * set up the local apic of whichever core is asking. every core has its
 * own set of these registers behind the same address, so this configures
 * the one running it and nobody else, which is the only way a core
 * that has just woken can prepare itself
 */
void lapic_enable_here(void);

bool lapic_send_init(uint32_t apic_id);
bool lapic_send_startup(uint32_t apic_id, uint8_t vector);

/*
 * an ordinary interrupt, from this core to another. how one processor
 * says anything at all to another once they are both running
 */
bool lapic_send_ipi(uint32_t apic_id, uint8_t vector);

/*
 * the timer, running at `hz`. it is driven by the bus clock, whose
 * speed nobody will tell the kernel, so it has to be measured against something
 * that already keeps time, which is what the pit is still good for
 */
void lapic_timer_start(uint32_t hz, uint64_t ticks_per_second);

/*
 * count the apic's own ticks over a known interval. `wait_ms` should
 * block for that long by some other means
 */
uint64_t lapic_calibrate(void (*wait_ms)(uint64_t), uint64_t ms);

#endif

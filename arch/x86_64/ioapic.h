// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/ioapic.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the io apic: where the machine's external interrupts arrive now that the
 * 8259 is out of the way.
 */

#ifndef ARCH_X86_64_IOAPIC_H
#define ARCH_X86_64_IOAPIC_H

#include <stdint.h>
#include <stdbool.h>

/*
 * the io apic: where the machine's external interrupts arrive now that
 * the 8259 is out of the way. it is a routing table, one entry per
 * incoming line, saying which vector to raise and on which cpu, and
 * that programmability is the whole difference from the old chip,
 * whose wiring was decided in 1981.
 */

bool ioapic_init(uint32_t phys_address, uint32_t gsi_base);
bool ioapic_available(void);

/*
 * send a global interrupt to a vector on a given lapic. `flags` are the
 * madt's polarity and trigger bits, which have to be honoured or a
 * level-triggered line will fire forever.
 *
 * returns false if the entry did not read back the way it was written.
 * that check is the only way to tell an mmio write that landed from one
 * that went nowhere, and getting it wrong here means a machine with
 * no keyboard, which cannot be recovered from by typing
 */
bool ioapic_route(uint32_t gsi, uint8_t vector, uint32_t lapic_id,
                  uint16_t flags);

void ioapic_mask(uint32_t gsi);

/* how many lines this one has, read from the chip itself */
uint32_t ioapic_lines(void);

#endif

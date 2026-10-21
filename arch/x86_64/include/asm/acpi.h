// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/acpi.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * enough acpi to find the interrupt controllers.
 */

#ifndef ARCH_X86_64_ASM_ACPI_H
#define ARCH_X86_64_ASM_ACPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * enough acpi to find the interrupt controllers.
 *
 * the chain is: the loader hands over the rsdp, which points at a table of
 * pointers to other tables, one of which is the madt, which says where
 * the local apic lives, where the io apics live, and which of the old
 * isa irq numbers have been quietly renumbered on this machine.
 *
 * that last part is the one that bites. the pit is wired to irq 0 and
 * everyone knows it, except that on most real machines it arrives at
 * the io apic on a different line, and the only way to know which is to
 * read the override entries. a kernel that assumes gets no timer.
 */

#define ACPI_MAX_IOAPICS  4
#define ACPI_MAX_OVERRIDES 16
#define ACPI_MAX_CPUS     32

struct acpi_ioapic {
    uint8_t  id;
    uint32_t address;       /* mmio, needs mapping, it is above ram */
    uint32_t gsi_base;      /* the first global interrupt it handles */
};

struct acpi_override {
    uint8_t  isa_irq;       /* what everybody calls it */
    uint32_t gsi;           /* where it actually arrives */
    uint16_t flags;         /* polarity and trigger mode */
};

struct acpi_info {
    bool     found;
    uint64_t lapic_address;
    size_t   cpu_count;             /* how many the firmware admits to */

    /*
     * and which they are. every core has a local apic with an id, and
     * that id is the only way to address one, there is no other name
     * for a cpu you have not started yet. the first entry is the core
     * reading this, by convention that firmware keeps
     */
    uint8_t  lapic_ids[ACPI_MAX_CPUS];

    struct acpi_ioapic ioapics[ACPI_MAX_IOAPICS];
    size_t   ioapic_count;

    struct acpi_override overrides[ACPI_MAX_OVERRIDES];
    size_t   override_count;
};

/*
 * walk from the rsdp to the madt and fill in what the kernel found. `read` maps
 * a physical address to something readable, the direct map in the
 * kernel, a test's own arena on a host
 */
struct acpi_info acpi_parse(uint64_t rsdp_phys, void *(*read)(uint64_t phys));

/*
 * checksum a table the way acpi specifies: every byte, summed, must
 * come to zero. split out because a wrong one means the table is not
 * what it claims and the kernel must not act on it
 */
bool acpi_checksum_ok(const void *table, size_t len);

/* where an isa irq really arrives, after any override */
uint32_t acpi_gsi_for_irq(const struct acpi_info *info, uint8_t isa_irq);

/* find the io apic that handles a given global interrupt */
const struct acpi_ioapic *acpi_ioapic_for_gsi(const struct acpi_info *info,
                                              uint32_t gsi);

/* read the tables the loader pointed the kernel at. returns what was found */
struct acpi_info acpi_init(void);

#endif

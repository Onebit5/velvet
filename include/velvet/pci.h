// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/pci.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what is plugged into this machine.
 */

#ifndef VELVET_PCI_H
#define VELVET_PCI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* what is plugged into this machine. */

#define PCI_MAX_DEVICES 32

struct pci_device {
    uint8_t  bus, slot, function;

    uint16_t vendor;
    uint16_t device;

    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  header_type;

    uint8_t  irq_line;
    uint32_t bar[6];
};

/* how a base address register is to be read. the low bits say which */
struct pci_bar {
    bool     is_io;         /* a port range rather than a memory window */
    bool     is_64bit;      /* this bar and the next one are one address */
    bool     prefetchable;
    uint64_t address;
};

/* config space, as a function, so a test can supply its own machine. */
typedef uint32_t (*pci_reader)(uint8_t bus, uint8_t slot, uint8_t fn,
                               uint8_t offset);

/* walk the buses and record what answers. */
size_t pci_scan_with(pci_reader read);

/* the same, against the real hardware */
void pci_scan(void);

/* every device the scan found, as `lspci` prints it and /proc/pci is. */
void pci_show(void);

/* /proc/pci. */
void pci_proc_init(void);

size_t pci_count(void);
const struct pci_device *pci_at(size_t index);



/* what sort of thing this is, in words. */
const char *pci_class_name(uint8_t class_code, uint8_t subclass);

/* who made it, if the kernel happens to know */
const char *pci_vendor_name(uint16_t vendor);

/* and what they called it. */
const char *pci_device_name(uint16_t vendor, uint16_t device);

struct pci_bar pci_decode_bar(uint32_t raw);

/* a card that only reads its own registers needs nothing from here. */

#define PCI_COMMAND 0x04

#define PCI_COMMAND_IO      (1u << 0)
#define PCI_COMMAND_MEMORY  (1u << 1)
#define PCI_COMMAND_MASTER  (1u << 2)   /* the one that matters */

/* and one that is switched off by being *set*. */
#define PCI_COMMAND_INTX_DISABLE (1u << 10)

/* set bits in a device's command register, leaving the rest alone. */
bool pci_enable(const struct pci_device *d, uint32_t bits);

/* and clear them. the same read-modify-write in the other direction */
bool pci_disable(const struct pci_device *d, uint32_t bits);

#endif

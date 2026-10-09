// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/pci.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * enumerating the pci bus.
 */

#include <velvet/pci.h>
#include <velvet/string.h>

/* offsets into configuration space. */
#define PCI_VENDOR      0x00
#define PCI_CLASS       0x08
#define PCI_HEADER      0x0c
#define PCI_BAR0        0x10
#define PCI_SECONDARY   0x18    /* on a bridge: the bus behind it */
#define PCI_IRQ         0x3c

#define HEADER_TYPE_MASK      0x7f
#define HEADER_MULTIFUNCTION  0x80
#define HEADER_BRIDGE         0x01

#define CLASS_BRIDGE          0x06
#define SUBCLASS_PCI_BRIDGE   0x04

static struct pci_device devices[PCI_MAX_DEVICES];
static size_t device_count;

size_t pci_count(void)
{
    return device_count;
}

const struct pci_device *pci_at(size_t index)
{
    return (index < device_count) ? &devices[index] : NULL;
}



static void scan_bus(pci_reader read, uint8_t bus, int depth);

static void record(pci_reader read, uint8_t bus, uint8_t slot, uint8_t fn,
                   int depth)
{
    if (device_count >= PCI_MAX_DEVICES) {
        return;
    }

    uint32_t id = read(bus, slot, fn, PCI_VENDOR);
    uint16_t vendor = id & 0xffff;

    /*
     * a slot nobody is in reads back all ones, because there is nothing
     * out there to pull the bus low
     */
    if (vendor == 0xffff || vendor == 0x0000) {
        return;
    }

    struct pci_device *d = &devices[device_count++];
    d->bus = bus;
    d->slot = slot;
    d->function = fn;
    d->vendor = vendor;
    d->device = (uint16_t)(id >> 16);

    uint32_t cls = read(bus, slot, fn, PCI_CLASS);
    d->prog_if    = (uint8_t)(cls >> 8);
    d->subclass   = (uint8_t)(cls >> 16);
    d->class_code = (uint8_t)(cls >> 24);

    d->header_type = (uint8_t)(read(bus, slot, fn, PCI_HEADER) >> 16);
    d->irq_line    = (uint8_t)read(bus, slot, fn, PCI_IRQ);

    /* a bridge has two bars, everything else has six. */
    size_t bars = ((d->header_type & HEADER_TYPE_MASK) == HEADER_BRIDGE) ? 2 : 6;
    for (size_t i = 0; i < 6; i++) {
        d->bar[i] = (i < bars)
                  ? read(bus, slot, fn, (uint8_t)(PCI_BAR0 + i * 4)) : 0;
    }

    /*
     * a bridge has another bus behind it, and the devices there are
     * every bit as real as the ones here
     */
    if ((d->header_type & HEADER_TYPE_MASK) == HEADER_BRIDGE
        && d->class_code == CLASS_BRIDGE
        && d->subclass == SUBCLASS_PCI_BRIDGE) {
        uint8_t secondary = (uint8_t)(read(bus, slot, fn, PCI_SECONDARY) >> 8);
        if (secondary != 0 && secondary != bus) {
            scan_bus(read, secondary, depth + 1);
        }
    }
}

static void scan_bus(pci_reader read, uint8_t bus, int depth)
{
    /* bridges are supposed to form a tree. */
    if (depth > 8) {
        return;
    }

    for (uint8_t slot = 0; slot < 32; slot++) {
        uint32_t id = read(bus, slot, 0, PCI_VENDOR);
        if ((id & 0xffff) == 0xffff) {
            continue;
        }

        record(read, bus, slot, 0, depth);

        /*
         * one physical part can present several devices, and only says
         * so in a bit on function zero. miss it and a sound card looks
         * like nothing at all
         */
        uint8_t header = (uint8_t)(read(bus, slot, 0, PCI_HEADER) >> 16);
        if (header & HEADER_MULTIFUNCTION) {
            for (uint8_t fn = 1; fn < 8; fn++) {
                record(read, bus, slot, fn, depth);
            }
        }
    }
}

size_t pci_scan_with(pci_reader read)
{
    device_count = 0;
    if (read != NULL) {
        scan_bus(read, 0, 0);
    }
    return device_count;
}



struct pci_bar pci_decode_bar(uint32_t raw)
{
    struct pci_bar b = { false, false, false, 0 };

    if (raw == 0) {
        return b;
    }

    if (raw & 1) {
        /* a port range. the low two bits are flags, the rest is a port */
        b.is_io = true;
        b.address = raw & ~0x3u;
        return b;
    }

    /*
     * a memory window. bits 1-2 say how wide the address is, bit 3
     * whether reading it twice may be optimised into once
     */
    /*
     * XXX: is_64bit is worked out here and then nothing acts on it. for
     * a wide window the top of the address lives in the next bar along,
     * which this function never sees and no caller joins back on, so a
     * device whose firmware parked it above 4 GiB gets mapped at
     * whatever the low half happened to be, usually zero, which is not a
     * window at all. the shell prints the flag next to an address that
     * is half of one, skips the bar entirely when the low half is zero,
     * and then prints the high half as though it were another bar. the
     * two dwords have to be joined before the address means anything.
     */
    b.is_64bit = ((raw >> 1) & 0x3) == 0x2;
    b.prefetchable = (raw >> 3) & 1;
    b.address = raw & ~0xfu;
    return b;
}

const char *pci_class_name(uint8_t class_code, uint8_t subclass)
{
    switch (class_code) {
    case 0x00: return "unclassified";
    case 0x01:
        switch (subclass) {
        case 0x00: return "scsi controller";
        case 0x01: return "ide controller";
        case 0x05: return "ata controller";
        case 0x06: return "sata controller";
        case 0x08: return "nvme controller";
        default:   return "storage controller";
        }
    case 0x02: return "network controller";
    case 0x03:
        return (subclass == 0x00) ? "vga display" : "display controller";
    case 0x04: return "multimedia";
    case 0x05: return "memory controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "host bridge";
        case 0x01: return "isa bridge";
        case 0x04: return "pci-to-pci bridge";
        default:   return "bridge";
        }
    case 0x07: return "communication controller";
    case 0x08: return "system peripheral";
    case 0x09: return "input device";
    case 0x0c:
        switch (subclass) {
        case 0x03: return "usb controller";
        case 0x05: return "smbus controller";
        default:   return "serial bus controller";
        }
    case 0x0d: return "wireless controller";
    default:   return "something the kernel has no name for";
    }
}

const char *pci_vendor_name(uint16_t vendor)
{
    switch (vendor) {
    case 0x8086: return "intel";
    case 0x1022: return "amd";
    case 0x10de: return "nvidia";
    case 0x1002: return "ati";
    case 0x1af4: return "virtio";
    case 0x1b36: return "qemu";
    case 0x1234: return "bochs";
    case 0x15ad: return "vmware";
    case 0x80ee: return "virtualbox";
    default:     return NULL;
    }
}

const char *pci_device_name(uint16_t vendor, uint16_t device)
{
    /* a very short list: the parts a machine like this actually has. */
    if (vendor == 0x8086) {
        switch (device) {
        case 0x1237: return "440FX host bridge";
        case 0x7000: return "PIIX3 ISA bridge";
        case 0x7010: return "PIIX3 IDE";
        case 0x7113: return "PIIX4 power management";
        case 0x29c0: return "Q35 host bridge";
        case 0x2918: return "ICH9 LPC bridge";
        case 0x2922: return "ICH9 SATA (AHCI)";
        case 0x2930: return "ICH9 SMBus";
        case 0x100e: return "82540EM gigabit ethernet";
        default: break;
        }
    }
    if (vendor == 0x1234 && device == 0x1111) {
        return "standard vga";
    }
    if (vendor == 0x1af4) {
        switch (device) {
        case 0x1000: return "virtio network";
        case 0x1001: return "virtio block";
        case 0x1002: return "virtio balloon";
        case 0x1003: return "virtio console";
        case 0x1005: return "virtio entropy";
        default: break;
        }
    }
    return NULL;
}

#ifndef VELVETOS_HOSTED

#include <asm/io.h>
#include <asm/irq.h>
#include <velvet/spinlock.h>

/*
 * the original way in, and still the one every machine supports: an
 * address written to one port, the answer read from another. there is a
 * memory-mapped version in newer firmware which reaches further into
 * config space, but nothing here needs the parts it can reach
 */
#define PCI_CONFIG_ADDRESS 0xcf8
#define PCI_CONFIG_DATA    0xcfc

/*
 * config space is an address port and a data port, and the pair is
 * only meaningful together, machine-wide, not per core
 */
static struct spinlock pci_lock = SPINLOCK("pci", LOCK_RANK_DEVICE);

static uint32_t port_read(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off)
{
    uint32_t address = (1u << 31)
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)fn   << 8)
                     | (off & 0xfc);

    /* two ports, one after the other, and nothing else may go between */
    uint64_t flags = spin_lock_irq(&pci_lock);
    outl(PCI_CONFIG_ADDRESS, address);
    uint32_t value = inl(PCI_CONFIG_DATA);
    spin_unlock_irq(&pci_lock, flags);

    return value;
}

static void port_write(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off,
                       uint32_t value)
{
    uint32_t address = (1u << 31)
                     | ((uint32_t)bus  << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)fn   << 8)
                     | (off & 0xfc);

    uint64_t flags = spin_lock_irq(&pci_lock);
    outl(PCI_CONFIG_ADDRESS, address);
    outl(PCI_CONFIG_DATA, value);
    spin_unlock_irq(&pci_lock, flags);
}

/*
 * read, or, write. the read-modify-write is the whole of it: the command
 * register holds several unrelated switches and a driver that wrote a
 * fresh value would turn off whatever the firmware had already arranged
 */
bool pci_enable(const struct pci_device *d, uint32_t bits)
{
    /*
     * XXX: this read-modify-write, and the one in pci_disable below, is
     * not one operation as far as the lock is concerned: port_read and
     * port_write each take pci_lock and let it go again, so two cores
     * setting different bits can read the same command word and then
     * overwrite one another, losing the first one's bit. it is not
     * reachable today, because the two callers both run from the boot
     * core one after the other, but the comment above is about exactly
     * this hazard and the code guards half of it. hold the lock across
     * the pair, the way port_read does for its own two ports.
     */
    uint32_t command = port_read(d->bus, d->slot, d->function, PCI_COMMAND);
    port_write(d->bus, d->slot, d->function, PCI_COMMAND, command | bits);

    /*
     * and read it back, because a device may simply not implement a bit
     * and saying so is better than wondering later why it moves nothing
     */
    command = port_read(d->bus, d->slot, d->function, PCI_COMMAND);
    return (command & bits) == bits;
}

bool pci_disable(const struct pci_device *d, uint32_t bits)
{
    uint32_t command = port_read(d->bus, d->slot, d->function, PCI_COMMAND);
    port_write(d->bus, d->slot, d->function, PCI_COMMAND, command & ~bits);

    command = port_read(d->bus, d->slot, d->function, PCI_COMMAND);
    return (command & bits) == 0;
}

void pci_scan(void)
{
    pci_scan_with(port_read);
}

#endif

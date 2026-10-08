// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_pci.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * enumerating the bus, against a machine of its own invention.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "drivers/pci.h"

static int failures;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); failures++; } } while (0)



struct fake {
    uint8_t bus, slot, fn;
    uint32_t id;        /* device << 16 | vendor */
    uint32_t class;     /* class << 24 | subclass << 16 | progif << 8 */
    uint32_t header;    /* header type in bits 16-23 */
    uint32_t secondary; /* bus behind a bridge, in bits 8-15 */
    uint32_t bar0;
    uint32_t irq;
};

static const struct fake machine[] = {
    /* a host bridge, on its own */
    { 0, 0, 0, 0x12378086, 0x06000000, 0x00000000, 0, 0,          0 },
    /* a vga card with a memory bar */
    { 0, 1, 0, 0x11111234, 0x03000000, 0x00000000, 0, 0xfd000008, 11 },
    /* a multifunction part: only function 0 says so */
    { 0, 2, 0, 0x29228086, 0x01060100, 0x00800000, 0, 0x0000c041, 10 },
    { 0, 2, 1, 0x29308086, 0x0c050000, 0x00000000, 0, 0x0000c081, 10 },
    /* a bridge to bus 5 */
    { 0, 3, 0, 0x24488086, 0x06040000, 0x00010000, 0x00000500, 0, 0 },
    /* and something behind it */
    { 5, 0, 0, 0x100e8086, 0x02000000, 0x00000000, 0, 0xfe000000, 5 },
};
#define MACHINE_SIZE (sizeof machine / sizeof machine[0])

static int reads;

static uint32_t fake_read(uint8_t bus, uint8_t slot, uint8_t fn, uint8_t off)
{
    reads++;
    for (size_t i = 0; i < MACHINE_SIZE; i++) {
        const struct fake *f = &machine[i];
        if (f->bus != bus || f->slot != slot || f->fn != fn) {
            continue;
        }
        switch (off) {
        case 0x00: return f->id;
        case 0x08: return f->class;
        case 0x0c: return f->header;
        case 0x10: return f->bar0;
        case 0x18: return f->secondary;
        case 0x3c: return f->irq;
        default:   return 0;
        }
    }
    return 0xffffffff;      /* an empty slot reads back all ones */
}

static const struct pci_device *find(uint8_t bus, uint8_t slot, uint8_t fn)
{
    for (size_t i = 0; ; i++) {
        const struct pci_device *d = pci_at(i);
        if (d == NULL) return NULL;
        if (d->bus == bus && d->slot == slot && d->function == fn) return d;
    }
}

int main(void)
{

    size_t n = pci_scan_with(fake_read);
    CHECK(n == MACHINE_SIZE, "every device on the machine was found");
    CHECK(pci_count() == n, "and counted");

    CHECK(find(0, 0, 0) != NULL, "the host bridge is there");
    CHECK(find(0, 1, 0) != NULL, "and the display");

    /* one physical part presenting two devices only admits it in a bit on function zero. */
    CHECK(find(0, 2, 0) != NULL, "a multifunction part's first function");
    CHECK(find(0, 2, 1) != NULL,
          "and its second, which is only reachable through the "
          "multifunction bit on the first");

    /*
     * a device on a bus behind a bridge is every bit as real as one in
     * front of it, and is only found by walking through
     */
    const struct pci_device *nic = find(5, 0, 0);
    CHECK(nic != NULL, "a device behind a bridge is found");
    CHECK(nic != NULL && nic->vendor == 0x8086 && nic->device == 0x100e,
          "and read correctly");

    /* a slot with nothing in it must not become a device */
    CHECK(find(0, 4, 0) == NULL, "an empty slot is not a device");
    CHECK(find(0, 1, 3) == NULL,
          "and a single-function device has no other functions");


    const struct pci_device *vga = find(0, 1, 0);
    CHECK(vga->vendor == 0x1234 && vga->device == 0x1111, "ids come out whole");
    CHECK(vga->class_code == 0x03 && vga->subclass == 0x00,
          "and the class and subclass are not swapped");
    CHECK(vga->irq_line == 11, "and the irq line");

    const struct pci_device *sata = find(0, 2, 0);
    CHECK(sata->class_code == 0x01 && sata->subclass == 0x06
          && sata->prog_if == 0x01, "prog-if is read too, not just the class");


    CHECK(pci_scan_with(NULL) == 0, "no way to read means no devices");
    CHECK(pci_at(0) == NULL, "and nothing to walk");
    pci_scan_with(fake_read);
    CHECK(pci_at(999) == NULL, "an index past the end gives nothing");


    {
        struct pci_bar b;

        b = pci_decode_bar(0xfd000008);
        CHECK(!b.is_io && b.address == 0xfd000000,
              "a memory bar has its flag bits masked off the address");
        CHECK(b.prefetchable, "and its prefetchable bit noticed");
        CHECK(!b.is_64bit, "a 32-bit window is not mistaken for a wide one");

        b = pci_decode_bar(0x0000c041);
        CHECK(b.is_io && b.address == 0xc040,
              "an io bar is a port range, with its low bits masked");

        b = pci_decode_bar(0xfe00000c);
        CHECK(b.is_64bit && b.prefetchable && b.address == 0xfe000000,
              "a 64-bit window says so in bits 1 and 2");

        b = pci_decode_bar(0);
        CHECK(b.address == 0 && !b.is_io, "an unused bar decodes to nothing");
    }


    CHECK(strcmp(pci_class_name(0x01, 0x06), "sata controller") == 0,
          "a subclass makes a vague class useful");
    CHECK(strcmp(pci_class_name(0x01, 0x08), "nvme controller") == 0, "nvme");
    CHECK(strcmp(pci_class_name(0x06, 0x00), "host bridge") == 0, "host bridge");
    CHECK(strcmp(pci_class_name(0x06, 0x04), "pci-to-pci bridge") == 0, "bridge");
    CHECK(strcmp(pci_class_name(0x02, 0x00), "network controller") == 0, "network");
    CHECK(pci_class_name(0xff, 0xff) != NULL,
          "and a class the test has never seen still gets words rather than a crash");
    CHECK(pci_class_name(0x01, 0xfe) != NULL, "as does an unknown subclass");

    CHECK(strcmp(pci_vendor_name(0x8086), "intel") == 0, "a vendor the test knows");
    CHECK(pci_vendor_name(0x9999) == NULL, "and one do not, honestly");

    CHECK(strcmp(pci_device_name(0x8086, 0x2922), "ICH9 SATA (AHCI)") == 0,
          "a device the test knows by name");
    CHECK(pci_device_name(0x8086, 0xdead) == NULL,
          "and one do not, the class still says what it is for");
    CHECK(pci_device_name(0x9999, 0x2922) == NULL,
          "a device id is only meaningful alongside its vendor");

    if (!failures) printf("all good\n");
    return failures;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * drivers/pciinfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what is plugged into the machine, to a person or in /proc.
 */

#include <velvet/pci.h>
#include <velvet/kprintf.h>
#include <velvet/proc.h>

/*
 * every device the scan found: where it is, who made it, what kind of
 * thing it is, where it listens and on which interrupt line
 */
void pci_show(void)
{
    if (pci_count() == 0) {
        kprintf("nothing answered on the pci bus\n");
        return;
    }

    for (size_t i = 0; ; i++) {
        const struct pci_device *d = pci_at(i);
        if (d == NULL) {
            break;
        }

        kprintf("  %02x:%02x.%u  %04x:%04x  %s",
                d->bus, d->slot, d->function, d->vendor, d->device,
                pci_class_name(d->class_code, d->subclass));

        const char *name = pci_device_name(d->vendor, d->device);
        const char *maker = pci_vendor_name(d->vendor);
        if (name != NULL) {
            kprintf(" , %s", name);
        } else if (maker != NULL) {
            kprintf(" , %s, model unknown to the kernel", maker);
        }
        kprintf("\n");

        /*
         * where it listens. a device with no bars is one that is
         * spoken to some other way, which is worth seeing too
         */
        bool any_bar = false;
        for (size_t b = 0; b < 6; b++) {
            struct pci_bar bar = pci_decode_bar(d->bar[b]);
            if (bar.address == 0) {
                continue;
            }
            if (!any_bar) {
                kprintf("           ");
                any_bar = true;
            }
            kprintf(" bar%zu=%s%p%s", b, bar.is_io ? "io " : "mem ",
                    (void *)bar.address, bar.is_64bit ? " (64-bit)" : "");
        }
        if (any_bar) {
            kprintf("\n");
        }
        if (d->irq_line != 0 && d->irq_line != 0xff) {
            kprintf("            irq %u\n", d->irq_line);
        }
    }
    kprintf("  %zu devices\n", pci_count());
}

void pci_proc_init(void)
{
    proc_register("pci", PROC_PUBLIC, pci_show);
}

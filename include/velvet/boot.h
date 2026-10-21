// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/boot.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the boot protocol: what a loader leaves the kernel. one struct, handed
 * over in rdi the way any function is called, describing everything about
 * this machine that could only be found out before long mode started.
 */

/* the protocol is described in docs/subsystems/boot.rst */

#ifndef VELVET_BOOT_H
#define VELVET_BOOT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * "PHLMN0", the magic of the loader the protocol was written with. it
 * stays, rather than taking a name of the kernel's, because every image
 * already written carries it
 */
#define BOOT_MAGIC    0x50484c4d4e30ull
#define BOOT_REVISION 1

/*
 * the direct map. all of physical memory appears again up here, so the
 * kernel can reach any address without building a mapping for it first
 */
#define BOOT_HHDM 0xffff800000000000ull

enum boot_memory {
    BOOT_MEM_USABLE = 0,
    BOOT_MEM_RESERVED,
    BOOT_MEM_ACPI_RECLAIMABLE,
    BOOT_MEM_ACPI_NVS,
    BOOT_MEM_BAD,
    BOOT_MEM_LOADER,    /* the loader's, the kernel's once it is done */
    BOOT_MEM_KERNEL,    /* the kernel and the ramdisk. never free */
};

struct boot_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct boot_framebuffer {
    uint64_t address;           /* through the direct map, ready to use */
    uint64_t pitch;
    uint32_t width, height, bpp;
    uint32_t red_shift, red_size;
    uint32_t green_shift, green_size;
    uint32_t blue_shift, blue_size;
};

struct boot_handoff {
    uint64_t magic;
    uint64_t revision;

    uint64_t hhdm;              /* add this to a physical address */

    uint64_t kernel_phys;       /* where the kernel really is */
    uint64_t kernel_virt;       /* and where it thinks it is */

    uint64_t memmap;            /* struct boot_memmap_entry *, sorted */
    uint64_t memmap_count;

    uint64_t ramdisk;           /* through the direct map */
    uint64_t ramdisk_size;

    uint64_t rsdp;              /* 0 if the firmware has no acpi tables */

    struct boot_framebuffer fb; /* width 0 means no video mode was got */
};

/*
 * the table a boot medium carries at block 32, saying where each piece is
 * on it. the kernel reads only the magic, to know which drive it was
 * booted from; the rest is the loader's, and the source archive's three
 * fields are for whatever runs later, zeroes when there is none
 */
#define BOOT_TABLE_LBA 32

struct boot_table {
    uint64_t magic;
    uint64_t handoff_lba, handoff_sectors;
    uint64_t kernel_lba,  kernel_sectors,  kernel_size;
    uint64_t ramdisk_lba, ramdisk_sectors, ramdisk_size;
    uint64_t source_lba,  source_sectors,  source_size;
};

void boot_take_handoff(const struct boot_handoff *h);
const struct boot_handoff *boot_handoff(void);

uint64_t boot_hhdm(void);

#endif

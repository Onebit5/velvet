// SPDX-License-Identifier: GPL-2.0-only
/*
 * boot/philemon.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * philemon: the bootloader.
 */

#ifndef BOOT_PHILEMON_H
#define BOOT_PHILEMON_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* philemon: the bootloader, named for the one who grants the power and then steps back. */

#define PHILEMON_MAGIC    0x50484c4d4e30ull      /* "PHLMN0" */
#define PHILEMON_REVISION 1

/*
 * the direct map. all of physical memory appears again up here, so the
 * kernel can reach any address without building a mapping for it first
 */
#define PHILEMON_HHDM 0xffff800000000000ull

/*
 * the assembly half has its own copy of these in philemon.inc, since
 * the two cannot share a header. tools/mkboot.py refuses to build if
 * they ever disagree.
 */

#define PH_LOADER_ADDR   0x00007c00ull   /* where the bios drops the first
                                          * sector, and where the rest of
                                          * the loader follows it */
#define PH_TABLE_ADDR    0x00000500ull   /* the disk layout table */
#define PH_HANDOFF_ADDR  0x00000600ull   /* what the kernel is handed */
#define PH_MEMMAP_ADDR   0x00001000ull   /* and the map it points at */
#define PH_BOUNCE_ADDR   0x00020000ull   /* the bios reads into here */
#define PH_HANDOFF64     0x00030000ull   /* the 64-bit half of the loader */
#define PH_VBE_ADDR      0x00060000ull
#define PH_E820_ADDR     0x00070000ull
#define PH_PAGETABLE     0x00200000ull
#define PH_KERNEL_IMAGE  0x00800000ull   /* the elf, still a file */
#define PH_RAMDISK_ADDR  0x01000000ull   /* the tar, where it stays */
#define PH_KERNEL_PHYS   0x02000000ull   /* where the kernel ends up */
#define PH_KERNEL_ROOM   (16 * 1024 * 1024)
#define PH_LOADER_ROOM   (8 * 1024 * 1024)  /* all of it reclaimable */



enum ph_memory {
    PH_MEM_USABLE = 0,
    PH_MEM_RESERVED,
    PH_MEM_ACPI_RECLAIMABLE,
    PH_MEM_ACPI_NVS,
    PH_MEM_BAD,
    PH_MEM_LOADER,      /* philemon's, and yours once you are done with this */
    PH_MEM_KERNEL,      /* the kernel and the ramdisk. never yours */
};

struct ph_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct ph_framebuffer {
    uint64_t address;           /* through the direct map, ready to use */
    uint64_t pitch;
    uint32_t width, height, bpp;
    uint32_t red_shift, red_size;
    uint32_t green_shift, green_size;
    uint32_t blue_shift, blue_size;
};

struct ph_handoff {
    uint64_t magic;
    uint64_t revision;

    uint64_t hhdm;              /* add this to a physical address */

    uint64_t kernel_phys;       /* where the kernel really is */
    uint64_t kernel_virt;       /* and where it thinks it is */

    uint64_t memmap;            /* struct ph_memmap_entry *, sorted */
    uint64_t memmap_count;

    uint64_t ramdisk;           /* through the direct map */
    uint64_t ramdisk_size;

    uint64_t rsdp;              /* 0 if the firmware has no acpi tables */

    struct ph_framebuffer fb;   /* width 0 means no video mode was got */
};

/*
 * a smaller, rawer thing than the handoff: this is the bios's own
 * account of the machine, before any of it has been made sense of.
 */

struct e820_entry {
    uint64_t base;
    uint64_t length;
    uint32_t type;
    uint32_t acpi_extended;
};

#define E820_USABLE       1
#define E820_RESERVED     2
#define E820_ACPI_RECLAIM 3
#define E820_ACPI_NVS     4
#define E820_BAD          5

struct ph_early {
    uint64_t magic;
    uint64_t kernel_image, kernel_size;
    uint64_t ramdisk, ramdisk_size;
    uint64_t e820, e820_count;
    uint64_t fb_address, fb_pitch;
    uint32_t fb_width, fb_height, fb_bpp;
    uint32_t fb_red_shift, fb_green_shift, fb_blue_shift;
    uint32_t fb_red_size, fb_green_size, fb_blue_size;
    uint64_t boot_drive;
};

/* the table mkboot.py writes into the image, saying where on the disk each piece is. */
#define PH_TABLE_LBA 32

struct ph_table {
    uint64_t magic;
    uint64_t handoff_lba, handoff_sectors;
    uint64_t kernel_lba,  kernel_sectors,  kernel_size;
    uint64_t ramdisk_lba, ramdisk_sectors, ramdisk_size;

    /* and the source this was all built from, which philemon does not load and never will. */
    uint64_t source_lba, source_sectors, source_size;
};



/*
 * copy an elf's loadable segments to where they belong, moving all of
 * them by the same amount so the distances the linker chose survive.
 * returns the entry point, or 0
 */
uint64_t ph_load_elf(const void *image, uint64_t size, uint64_t phys_at,
                     uint64_t *phys_base, uint64_t *virt_base,
                     const char **error);

/*
 * turn the bios's account of memory into the kernel's, with everything
 * already spent carved out of it. returns how many entries were written
 */
uint64_t ph_build_memmap(const struct e820_entry *bios, uint64_t bios_count,
                         uint64_t ramdisk, uint64_t ramdisk_size,
                         struct ph_memmap_entry *out, uint64_t out_max);

/* find the acpi tables where the firmware leaves them */
uint64_t ph_find_rsdp(const void *ebda, const void *bios_area);

#endif

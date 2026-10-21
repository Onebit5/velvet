// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_boot.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the boot protocol as the kernel declares it, held against the loader's
 * own declaration of the same bytes.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include <velvet/boot.h>
#include <philemon.h>

static int s_i_failures;

#define CHECK(cond, what) do {                                          \
        if (!(cond)) {                                                  \
            printf("  FAIL %s (%s:%d)\n", what, __FILE__, __LINE__);     \
            s_i_failures++;                                             \
        }                                                               \
    } while (0)

/* one field, at the same place and of the same size in both */
#define SAME_FIELD(ours, theirs, field)                                 \
    CHECK(offsetof(struct ours, field) == offsetof(struct theirs, field) \
          && sizeof(((struct ours *)0)->field)                          \
             == sizeof(((struct theirs *)0)->field),                    \
          #ours "." #field " is where the loader puts it")

#define SAME_SIZE(ours, theirs)                                         \
    CHECK(sizeof(struct ours) == sizeof(struct theirs),                 \
          #ours " is as long as the loader's")

static void test_constants(void)
{
    CHECK(BOOT_MAGIC == PHILEMON_MAGIC, "the magic is the loader's");
    CHECK(BOOT_REVISION == PHILEMON_REVISION, "and so is the revision");
    CHECK(BOOT_HHDM == PHILEMON_HHDM, "the direct map is where it maps it");
    CHECK(BOOT_TABLE_LBA == PH_TABLE_LBA, "the table is where it writes it");

    /* two enums, so compared as the numbers they go over as */
    CHECK((int)BOOT_MEM_USABLE == (int)PH_MEM_USABLE
          && (int)BOOT_MEM_RESERVED == (int)PH_MEM_RESERVED
          && (int)BOOT_MEM_ACPI_RECLAIMABLE == (int)PH_MEM_ACPI_RECLAIMABLE
          && (int)BOOT_MEM_ACPI_NVS == (int)PH_MEM_ACPI_NVS
          && (int)BOOT_MEM_BAD == (int)PH_MEM_BAD
          && (int)BOOT_MEM_LOADER == (int)PH_MEM_LOADER
          && (int)BOOT_MEM_KERNEL == (int)PH_MEM_KERNEL,
          "every kind of memory has the number the loader gives it");
}

static void test_handoff(void)
{
    SAME_FIELD(boot_handoff, ph_handoff, magic);
    SAME_FIELD(boot_handoff, ph_handoff, revision);
    SAME_FIELD(boot_handoff, ph_handoff, hhdm);
    SAME_FIELD(boot_handoff, ph_handoff, kernel_phys);
    SAME_FIELD(boot_handoff, ph_handoff, kernel_virt);
    SAME_FIELD(boot_handoff, ph_handoff, memmap);
    SAME_FIELD(boot_handoff, ph_handoff, memmap_count);
    SAME_FIELD(boot_handoff, ph_handoff, ramdisk);
    SAME_FIELD(boot_handoff, ph_handoff, ramdisk_size);
    SAME_FIELD(boot_handoff, ph_handoff, rsdp);
    SAME_FIELD(boot_handoff, ph_handoff, fb);
    SAME_SIZE(boot_handoff, ph_handoff);

    SAME_FIELD(boot_framebuffer, ph_framebuffer, address);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, pitch);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, width);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, height);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, bpp);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, red_shift);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, red_size);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, green_shift);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, green_size);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, blue_shift);
    SAME_FIELD(boot_framebuffer, ph_framebuffer, blue_size);
    SAME_SIZE(boot_framebuffer, ph_framebuffer);

    SAME_FIELD(boot_memmap_entry, ph_memmap_entry, base);
    SAME_FIELD(boot_memmap_entry, ph_memmap_entry, length);
    SAME_FIELD(boot_memmap_entry, ph_memmap_entry, type);
    SAME_SIZE(boot_memmap_entry, ph_memmap_entry);
}

static void test_table(void)
{
    SAME_FIELD(boot_table, ph_table, magic);
    SAME_FIELD(boot_table, ph_table, handoff_lba);
    SAME_FIELD(boot_table, ph_table, handoff_sectors);
    SAME_FIELD(boot_table, ph_table, kernel_lba);
    SAME_FIELD(boot_table, ph_table, kernel_sectors);
    SAME_FIELD(boot_table, ph_table, kernel_size);
    SAME_FIELD(boot_table, ph_table, ramdisk_lba);
    SAME_FIELD(boot_table, ph_table, ramdisk_sectors);
    SAME_FIELD(boot_table, ph_table, ramdisk_size);
    SAME_FIELD(boot_table, ph_table, source_lba);
    SAME_FIELD(boot_table, ph_table, source_sectors);
    SAME_FIELD(boot_table, ph_table, source_size);
    SAME_SIZE(boot_table, ph_table);
}

int main(void)
{
    test_constants();
    test_handoff();
    test_table();

    if (s_i_failures > 0) {
        printf("  %d failed\n", s_i_failures);
        return 1;
    }
    return 0;
}

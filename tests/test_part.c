// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_part.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for partition tables.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>

#include "drivers/part.h"
#include "lib/hash.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)



#define DISK_SECTORS 256
static uint8_t disk[DISK_SECTORS * PART_SECTOR];
static bool disk_refuses;

static bool mem_write(void *ctx, uint64_t lba, uint32_t count,
                      const void *buf)
{
    (void)ctx;
    if (disk_refuses) return false;
    if (lba + count > DISK_SECTORS) return false;
    memcpy(&disk[lba * PART_SECTOR], buf, (size_t)count * PART_SECTOR);
    return true;
}

static bool mem_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (disk_refuses) return false;
    if (lba + count > DISK_SECTORS) return false;
    memcpy(buf, &disk[lba * PART_SECTOR], (size_t)count * PART_SECTOR);
    return true;
}



static int img_fd = -1;
static bool file_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    ssize_t n = pread(img_fd, buf, (size_t)count * PART_SECTOR,
                      (off_t)(lba * PART_SECTOR));
    return n == (ssize_t)(count * PART_SECTOR);
}



static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static void blank_disk(void)
{
    memset(disk, 0, sizeof disk);
    disk_refuses = false;
}

static void mbr_entry(int slot, uint8_t type, uint32_t first, uint32_t count,
                      bool boot)
{
    uint8_t *e = disk + 446 + slot * 16;
    e[0] = boot ? 0x80 : 0x00;
    e[4] = type;
    wr32(e + 8, first);
    wr32(e + 12, count);
}

static void sign_mbr(void)
{
    disk[510] = 0x55;
    disk[511] = 0xaa;
}

/* a gpt with `count` entries, written at sector 1 and 2 onwards */
static void build_gpt(int count, bool break_header_crc, bool break_entry_crc)
{
    uint32_t entry_size = 128;
    uint32_t entry_count = 8;
    uint32_t entries_bytes = entry_count * entry_size;

    uint8_t *entries = disk + 2 * PART_SECTOR;
    memset(entries, 0, entries_bytes);

    for (int i = 0; i < count; i++) {
        uint8_t *e = entries + i * entry_size;
        /* the linux filesystem type guid */
        static const uint8_t linux_guid[16] = {
            0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
            0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };
        memcpy(e, linux_guid, 16);
        memset(e + 16, 0x11 + i, 16);
        wr64(e + 32, 34 + (uint64_t)i * 20);
        wr64(e + 40, 34 + (uint64_t)i * 20 + 19);

        /* a name, in utf-16 */
        const char *name = (i == 0) ? "root" : "spare";
        for (int c = 0; name[c] != '\0'; c++) {
            e[56 + c * 2] = (uint8_t)name[c];
            e[57 + c * 2] = 0;
        }
    }

    uint32_t entries_crc = crc32_of(entries, entries_bytes);
    if (break_entry_crc) {
        entries_crc ^= 1;
    }

    uint8_t *h = disk + PART_SECTOR;
    memset(h, 0, PART_SECTOR);
    memcpy(h, "EFI PART", 8);
    wr32(h + 8, 0x00010000);
    wr32(h + 12, 92);
    wr32(h + 16, 0);
    wr64(h + 24, 1);
    wr64(h + 32, DISK_SECTORS - 1);
    wr64(h + 40, 34);
    wr64(h + 48, DISK_SECTORS - 34);
    wr64(h + 72, 2);
    wr32(h + 80, entry_count);
    wr32(h + 84, entry_size);
    wr32(h + 88, entries_crc);
    wr32(h + 16, crc32_of(h, 92));

    if (break_header_crc) {
        h[16] ^= 1;
    }

    /* the protective entry that makes this a gpt disk */
    mbr_entry(0, 0xee, 1, DISK_SECTORS - 1, false);
    sign_mbr();
}

int main(void)
{
    struct partition parts[PART_MAX];
    enum part_scheme scheme;
    size_t n;

    /* this one vector is what anchors the gpt half of this to the outside world. */
    CHECK(crc32_of("123456789", 9) == 0xcbf43926u,
          "crc32 of \"123456789\" is 0xcbf43926, as it is everywhere");
    CHECK(crc32_of("", 0) == 0, "and of nothing is nothing");
    CHECK(crc32_of("a", 1) == 0xe8b7be43u, "and of \"a\" is 0xe8b7be43");



    blank_disk();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0 && scheme == PART_NONE,
          "a disk with no signature has no partitions");

    /*
     * that is not a failure, and the difference matters: plenty of
     * images are one filesystem written straight to sector zero, and
     * the caller has to be able to tell "no table" from "bad table"
     */

    blank_disk();
    disk_refuses = true;
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0 && scheme == PART_NONE, "and a drive that will not read has none");



    blank_disk();
    mbr_entry(0, 0x83, 2048, 8192, true);
    mbr_entry(1, 0x0c, 10240, 4096, false);
    sign_mbr();

    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 2 && scheme == PART_MBR, "an mbr with two entries reads as two");
    CHECK(parts[0].first_lba == 2048 && parts[0].sectors == 8192,
          "with the first one's place and size");
    CHECK(parts[0].index == 1, "numbered from one, the way everybody does");
    CHECK(parts[0].bootable, "and the active flag");
    CHECK(strcmp(parts[0].kind, "linux") == 0, "and what the type byte means");
    CHECK(parts[1].first_lba == 10240 && !parts[1].bootable,
          "and the second one too");
    CHECK(strcmp(parts[1].kind, "fat32") == 0, "with its own type");
    CHECK(parts[0].name[0] == '\0',
          "and no name, because mbr has nowhere to keep one, an empty "
          "string rather than an invented one");

    /* empty slots in the middle are skipped, and the ones after them are still found. */
    blank_disk();
    mbr_entry(0, 0x00, 0, 0, false);
    mbr_entry(1, 0x83, 100, 200, false);
    mbr_entry(3, 0x83, 500, 600, false);
    sign_mbr();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 2, "empty slots are skipped rather than ending the table");
    CHECK(parts[0].first_lba == 100 && parts[1].first_lba == 500,
          "and what is after them is still found");
    CHECK(parts[0].index == 2 && parts[1].index == 4,
          "keeping the slot numbers they really occupy, since that is what "
          "anybody else calls them");

    /* an entry with a length of zero describes nothing */
    blank_disk();
    mbr_entry(0, 0x83, 2048, 0, false);
    sign_mbr();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "an entry of zero length is not a partition");

    /* extended partitions are listed, not walked */
    blank_disk();
    mbr_entry(0, 0x05, 2048, 8192, false);
    sign_mbr();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 1 && strcmp(parts[0].kind, "extended") == 0,
          "an extended partition is listed as one");



    blank_disk();
    build_gpt(2, false, false);
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 2 && scheme == PART_GPT, "a gpt disk reads as gpt");
    CHECK(parts[0].first_lba == 34 && parts[0].sectors == 20,
          "with 64-bit addresses, and a length worked out from the last "
          "sector rather than stored");
    CHECK(strcmp(parts[0].name, "root") == 0,
          "and the name, which is the thing mbr could not do");
    CHECK(strcmp(parts[1].name, "spare") == 0, "for each of them");
    CHECK(strcmp(parts[0].kind, "linux") == 0,
          "and the type, read from a guid rather than a byte");
    CHECK(parts[0].mbr_type == 0,
          "with no mbr type, because there is no mbr entry behind it");

    /* the protective mbr must not be reported as a partition. */
    bool any_protective = false;
    for (size_t i = 0; i < n; i++) {
        if (parts[i].first_lba == 1) {
            any_protective = true;
        }
    }
    CHECK(!any_protective,
          "and the protective mbr entry is not among them, it covers the "
          "whole disk, so following it would mount the table");

    /* this is the half that matters. */

    blank_disk();
    build_gpt(2, true, false);
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "a gpt header whose crc is wrong is refused");
    CHECK(scheme == PART_NONE, "and reported as no table rather than a bad one");

    blank_disk();
    build_gpt(2, false, true);
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0,
          "and so is one whose *entries* do not add up, even though its "
          "header does");

    /* a protective mbr with nothing behind it. */
    blank_disk();
    mbr_entry(0, 0xee, 1, DISK_SECTORS - 1, false);
    sign_mbr();
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "a protective mbr with no gpt behind it has no partitions");

    /* nonsense in the header's own fields */
    blank_disk();
    build_gpt(1, false, false);
    disk[PART_SECTOR + 84] = 0;         /* entry size of zero */
    wr32(disk + PART_SECTOR + 16, 0);
    wr32(disk + PART_SECTOR + 16, crc32_of(disk + PART_SECTOR, 92));
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "an entry size of zero is refused rather than divided by");

    /* an entry whose last sector is before its first */
    blank_disk();
    build_gpt(1, false, false);
    {
        uint8_t *e = disk + 2 * PART_SECTOR;
        wr64(e + 40, 10);               /* last, now below first */
        wr32(disk + PART_SECTOR + 88, crc32_of(disk + 2 * PART_SECTOR, 8 * 128));
        wr32(disk + PART_SECTOR + 16, 0);
        wr32(disk + PART_SECTOR + 16, crc32_of(disk + PART_SECTOR, 92));
    }
    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 0, "and an entry that ends before it starts is skipped");



    for (int which = 0; which < 2; which++) {
        const char *path = which ? "bin/tests/gpt.img" : "bin/tests/mbr.img";
        img_fd = open(path, O_RDONLY);
        if (img_fd < 0) {
            printf("FAIL: no %s, `make part-images`\n", path);
            failures++;
            continue;
        }

        n = part_scan(file_read, NULL, parts, PART_MAX, &scheme);
        CHECK(n == 2, which ? "the real gpt image has two partitions"
                            : "the real mbr image has two partitions");
        CHECK(scheme == (which ? PART_GPT : PART_MBR),
              "of the scheme it was built with");
        if (n == 2) {
            CHECK(parts[0].first_lba == 2048,
                  "the first aligned to a mebibyte, as every tool has since "
                  "disks stopped having real cylinders");
            CHECK(parts[1].first_lba > parts[0].first_lba
                  && parts[1].first_lba >= parts[0].first_lba + parts[0].sectors,
                  "and the second after it, not overlapping");
        }
        close(img_fd);
        img_fd = -1;
    }

    /*
     * the round trip is the whole test: what part_write_mbr lays down,
     * part_scan has to read back as the same partitions. the two were
     * written a version apart and share nothing but the layout, which is
     * the only reason their agreeing means anything.
     *
     * the disk starts with something in its first 446 bytes, because on
     * a real machine that is philemon, the table lives *inside* the
     * boot sector, which is the whole awkwardness of the mbr
     */

    disk_refuses = false;
    memset(disk, 0, sizeof disk);
    for (int i = 0; i < 446; i++) {
        disk[i] = (uint8_t)(0x90 + (i & 7));     /* a stand-in bootloader */
    }

    struct part_plan plan[2] = {
        { .first_lba = 64,  .sectors = 64, .type = 0x83, .bootable = true  },
        { .first_lba = 128, .sectors = 64, .type = 0x0c, .bootable = false },
    };
    const char *why = NULL;

    CHECK(part_write_mbr(mem_read, mem_write, NULL, DISK_SECTORS, plan, 2, &why),
          "a table of two partitions is written");
    CHECK(why == NULL, "with nothing to report");
    CHECK(disk[510] == 0x55 && disk[511] == 0xaa,
          "and the signature is there, which is what makes it a table "
          "rather than 512 bytes that happen to be sitting there");

    bool boot_intact = true;
    for (int i = 0; i < 446; i++) {
        if (disk[i] != (uint8_t)(0x90 + (i & 7))) { boot_intact = false; break; }
    }
    CHECK(boot_intact,
          "and the first 446 bytes are untouched, a writer that laid "
          "down a fresh sector would partition the disk and erase the "
          "bootloader in the same instruction");

    n = part_scan(mem_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 2, "the scanner finds both of them again");
    CHECK(scheme == PART_MBR, "and calls it an mbr");
    CHECK(parts[0].first_lba == 64 && parts[0].sectors == 64,
          "the first is where it was put");
    CHECK(parts[0].mbr_type == 0x83 && parts[0].bootable,
          "with its type and its active flag");
    CHECK(parts[1].first_lba == 128 && parts[1].sectors == 64,
          "and so is the second");
    CHECK(!parts[1].bootable, "which is not the bootable one");
    CHECK(parts[0].index == 1 && parts[1].index == 2,
          "numbered from one, the way everybody numbers them");



    struct part_plan bad = { .first_lba = 250, .sectors = 100,
                             .type = 0x83, .bootable = false };
    CHECK(!part_write_mbr(mem_read, mem_write, NULL, DISK_SECTORS, &bad, 1, &why),
          "a partition running off the end is refused");

    bad.first_lba = 0; bad.sectors = 32;
    CHECK(!part_write_mbr(mem_read, mem_write, NULL, DISK_SECTORS, &bad, 1, &why),
          "and so is one starting at sector zero, where the table lives");

    struct part_plan overlap[2] = {
        { .first_lba = 64, .sectors = 64, .type = 0x83 },
        { .first_lba = 96, .sectors = 64, .type = 0x83 },
    };
    CHECK(!part_write_mbr(mem_read, mem_write, NULL, DISK_SECTORS, overlap, 2, &why),
          "two that overlap are refused rather than written");
    CHECK(!part_write_mbr(mem_read, mem_write, NULL, DISK_SECTORS, plan, 5, &why),
          "and an mbr holds four, not five");

    /* the important refusal. */
    memset(disk, 0, sizeof disk);
    disk[446 + 4] = 0xee;
    disk[510] = 0x55; disk[511] = 0xaa;
    CHECK(!part_write_mbr(mem_read, mem_write, NULL, DISK_SECTORS, plan, 1, &why),
          "a disk carrying a gpt is left alone");
    CHECK(why != NULL, "and says why rather than failing quietly");

    if (failures == 0) printf("all good\n");
    return failures;
}

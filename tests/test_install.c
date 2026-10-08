// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_install.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for installing.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "fs/install.h"
#include "fs/ext4.h"
#include "drivers/part.h"
#include "philemon.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)



#define SRC_SECTORS  4096           /* 2 MiB of boot medium */
#define DST_SECTORS  262144         /* 128 MiB of target */

static uint8_t *src, *dst;
static bool dst_refuses;

static bool src_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (lba + count > SRC_SECTORS) return false;
    memcpy(buf, src + lba * 512, (size_t)count * 512);
    return true;
}

static bool dst_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (lba + count > DST_SECTORS) return false;
    memcpy(buf, dst + lba * 512, (size_t)count * 512);
    return true;
}

static bool dst_write(void *ctx, uint64_t lba, uint32_t count,
                      const void *buf)
{
    (void)ctx;
    if (dst_refuses) return false;
    if (lba + count > DST_SECTORS) return false;
    memcpy(dst + lba * 512, buf, (size_t)count * 512);
    return true;
}

/*
 * the ext2 driver wants a writer with the same shape, shifted into the
 * partition, which is exactly what a mounted filesystem gets
 */
static uint64_t view_first;
static bool view_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    return dst_read(ctx, view_first + lba, count, buf);
}
static bool view_write(void *ctx, uint64_t lba, uint32_t count,
                       const void *buf)
{
    return dst_write(ctx, view_first + lba, count, buf);
}

static int said;
static void note(void *ctx, const char *what)
{
    (void)ctx; (void)what; said++;
}

/*
 * a boot medium, made the way tools/mkboot.py makes one: philemon in
 * the first sectors, his table at 32, then the pieces
 */
static void make_medium(uint64_t magic)
{
    memset(src, 0, SRC_SECTORS * 512);

    /* philemon's first stage. */
    for (int i = 0; i < 366; i++) {
        src[i] = (uint8_t)(0x60 + (i & 15));
    }
    src[510] = 0x55;
    src[511] = 0xaa;

    struct ph_table t;
    memset(&t, 0, sizeof t);
    t.magic           = magic;
    t.handoff_lba     = 33;
    t.handoff_sectors = 8;
    t.kernel_lba      = 41;
    t.kernel_sectors  = 1200;
    t.kernel_size     = 1200 * 512;
    t.ramdisk_lba     = 1241;
    t.ramdisk_sectors = 640;
    t.ramdisk_size    = 640 * 512;
    /*
     * and the source tree, which philemon never loads and the installer
     * has to copy anyway: it is on the medium, and a copy that stopped
     * at the ramdisk would make a disk that boots and can no longer say
     * what it is made of
     */
    t.source_lba      = 1881;
    t.source_sectors  = 400;
    t.source_size     = 400 * 512;
    memcpy(src + PH_TABLE_LBA * 512, &t, sizeof t);

    /*
     * something recognisable where the kernel, the ramdisk and the
     * source go, so a copy that is off by a sector can be seen to be
     * off by a sector
     */
    for (uint64_t s = t.kernel_lba; s < t.source_lba + t.source_sectors; s++) {
        memset(src + s * 512, (int)(s & 0xff), 512);
    }
}

/*
 * the same medium as an older mkboot.py would have written it: the
 * table stops after the ramdisk and the three fields behind it are
 * zero, which is the only thing a field appended to a struct can mean
 */
static void make_medium_without_source(void)
{
    make_medium(PHILEMON_MAGIC);
    struct ph_table t;
    memcpy(&t, src + PH_TABLE_LBA * 512, sizeof t);
    t.source_lba = t.source_sectors = t.source_size = 0;
    memcpy(src + PH_TABLE_LBA * 512, &t, sizeof t);
}

int main(void)
{
    src = malloc(SRC_SECTORS * 512);
    dst = malloc(DST_SECTORS * 512);

    struct install_io io = {
        .src_read = src_read, .dst_read = dst_read, .dst_write = dst_write,
        .ctx = NULL, .dst_sectors = DST_SECTORS, .say = note,
    };
    struct install_result r;
    const char *error = NULL;



    make_medium(0);                 /* no magic: not a boot medium */
    memset(dst, 0xa5, DST_SECTORS * 512);
    CHECK(!install_system(&io, 0, true, &r, &error),
          "a medium with no table is refused");
    CHECK(error != NULL, "and says so");
    CHECK(install_system_end(src_read, NULL) == 0,
          "and reads as no system at all");

    make_medium(PHILEMON_MAGIC);
    CHECK(install_system_end(src_read, NULL) == 1881 + 400,
          "a real one says where the system ends, which is the end of the "
          "source tree and not the end of the medium");

    make_medium_without_source();
    CHECK(install_system_end(src_read, NULL) == 1241 + 640,
          "and an image from before there was a source tree ends where it "
          "always did, rather than at sector zero");
    make_medium(PHILEMON_MAGIC);

    struct install_io tiny = io;
    tiny.dst_sectors = 2000;        /* smaller than the system itself */
    CHECK(!install_system(&tiny, 0, true, &r, &error),
          "a target too small for the system is refused");



    memset(dst, 0xa5, DST_SECTORS * 512);
    dst_refuses = false;
    said = 0;
    error = NULL;
    CHECK(install_system(&io, 1700000000, true, &r, &error), "it installs");
    CHECK(error == NULL, "with nothing to report");
    CHECK(said >= 3, "and said what it was doing as it went");

    CHECK(r.system_sectors == 2281,
          "the system is as long as the table said, source included");
    CHECK(r.part_first_lba == 4096,
          "and the partition starts on the first megabyte boundary past "
          "it, which moved when the source went into the system area, "
          "and is arithmetic rather than a number anybody chose");

    /*
     * one filesystem means that anything filling it stops the machine
     * as well: no room for the work is also no room for the system.
     * so the installer lays down two, and the split is not half and
     * half, what the system needs is bounded and known, and what the
     * work needs is whatever is left
     */
    CHECK(r.work_sectors > 0,
          "a disk this size is split, so that filling one filesystem does "
          "not leave the system with nowhere to write");
    CHECK(r.part_sectors + r.work_sectors == DST_SECTORS - r.part_first_lba,
          "and the two of them take the whole of what is left, with "
          "nothing stranded between");
    CHECK(r.work_first_lba == r.part_first_lba + r.part_sectors,
          "the second begins exactly where the first ends");
    CHECK(r.work_sectors > r.part_sectors,
          "and is the larger of the two, the system's needs are known "
          "and the work's are not");
    CHECK(r.work_first_lba % 2048 == 0,
          "and it is aligned like the first, for the same reason nobody "
          "asks the drive about");



    /* philemon is still there. */
    bool loader_intact = true;
    for (int i = 0; i < 366; i++) {
        if (dst[i] != (uint8_t)(0x60 + (i & 15))) { loader_intact = false; break; }
    }
    CHECK(loader_intact,
          "philemon survived the partition table being written over the "
          "back half of his own sector");
    CHECK(dst[510] == 0x55 && dst[511] == 0xaa, "and the signature is intact");

    /*
     * his table came across, so he will find it on the new disk, he
     * scans drives for the magic rather than being told which drive he
     * is on, which is the whole reason any of this works
     */
    struct ph_table copied;
    memcpy(&copied, dst + PH_TABLE_LBA * 512, sizeof copied);
    CHECK(copied.magic == PHILEMON_MAGIC, "the boot table came across");
    CHECK(copied.kernel_lba == 41 && copied.ramdisk_sectors == 640,
          "unchanged, because the copy is verbatim rather than rebuilt");
    CHECK(copied.source_lba == 1881 && copied.source_size == 400 * 512,
          "and it still says where the source is, at the same sectors, "
          "because they are the same sectors");

    /* and the kernel and ramdisk bytes are where they were */
    bool payload_ok = true;
    for (uint64_t s = 41; s < 2281; s++) {
        if (dst[s * 512] != (uint8_t)(s & 0xff)) { payload_ok = false; break; }
    }
    CHECK(payload_ok,
          "and every sector of the kernel, the ramdisk and the source "
          "landed at the lba it started at, a copy that shifted by one "
          "would boot to nothing and say nothing about why");

    /* the partition scanner finds what was written */
    struct partition parts[PART_MAX];
    enum part_scheme scheme = PART_NONE;
    size_t n = part_scan(dst_read, NULL, parts, PART_MAX, &scheme);
    CHECK(n == 2 && scheme == PART_MBR, "there are two partitions, in an mbr");
    CHECK(parts[0].first_lba == r.part_first_lba, "where the installer said");
    CHECK(parts[0].mbr_type == 0x83, "typed as linux, which is what ext2 is");
    CHECK(parts[1].first_lba == r.work_first_lba, "and so is the second");
    CHECK(parts[0].bootable && !parts[1].bootable,
          "only the first is bootable, because only the first is booted "
          "from");

    /* and both filesystems mount */
    view_first = parts[0].first_lba;
    static struct ext4 fs;
    CHECK(ext4_mount(&fs, view_read, view_write, NULL),
          "the ext2 driver mounts the new partition");

    struct ext4_file f;
    CHECK(ext4_lookup(&fs, "/", &f) && f.is_dir,
          "and there is a root directory to put a system in");

    view_first = parts[1].first_lba;
    static struct ext4 work;
    CHECK(ext4_mount(&work, view_read, view_write, NULL),
          "and the second one mounts too");
    CHECK(ext4_lookup(&work, "/", &f) && f.is_dir,
          "with a root directory of its own");
    CHECK(strcmp(work.label, "work") == 0,
          "labelled for what it is, so that `mount` can say which is "
          "which without counting partitions");
    CHECK(work.blocks_count == r.work_blocks,
          "and as big as the installer said it made it");

    /*
     * two filesystems each too small to hold anything is worse than one
     * that is merely small, so the split gives way rather than the
     * install failing, and the result says which happened
     */
    {
        struct install_io small = io;
        small.dst_sectors = 40960;      /* 20 MiB */
        memset(dst, 0xa5, DST_SECTORS * 512);
        error = NULL;
        struct install_result sr;
        CHECK(install_system(&small, 1700000000, true, &sr, &error),
              "a small disk still installs");
        CHECK(sr.work_sectors == 0,
              "and is not split, because there is not room for two worth "
              "having");
        CHECK(sr.part_sectors == 40960 - sr.part_first_lba,
              "the one filesystem takes all of what is left");
    }

    /* and asking for one filesystem gets one, whatever the disk */
    {
        memset(dst, 0xa5, DST_SECTORS * 512);
        error = NULL;
        struct install_result wr;
        CHECK(install_system(&io, 1700000000, false, &wr, &error),
              "a whole-disk install works");
        CHECK(wr.work_sectors == 0, "and lays down one partition");
        CHECK(wr.part_sectors == DST_SECTORS - wr.part_first_lba,
              "covering all of it");
    }

    /*
     * put the two-partition install back, so what follows is checking
     * what the ordinary path produced
     */
    memset(dst, 0xa5, DST_SECTORS * 512);
    error = NULL;
    CHECK(install_system(&io, 1700000000, true, &r, &error),
          "and the ordinary install goes back down");

    /*
     * an installer that reports success on a disk it only half wrote is
     * worse than one that cannot install at all: the machine looks ready
     * and is not
     */
    memset(dst, 0xa5, DST_SECTORS * 512);
    dst_refuses = true;
    error = NULL;
    CHECK(!install_system(&io, 0, true, &r, &error),
          "a drive that refuses writes fails the install");
    CHECK(error != NULL, "and says which part gave up");
    dst_refuses = false;

    if (failures == 0) printf("all good\n");
    free(src);
    free(dst);
    return failures;
}

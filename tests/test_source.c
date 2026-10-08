// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_source.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the source tree that stays on the medium.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include "fs/source.h"
#include "fs/ustar.h"
#include "philemon.h"
#include "lib/hash.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)



#define SECTOR      512
#define MEDIUM_SECTORS 512

static uint8_t *medium;
static int reads;           /* how many times the drive was touched */
static bool medium_broken;  /* a drive that stops answering */

static bool medium_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    reads++;
    if (medium_broken) {
        return false;
    }
    if (lba + count > MEDIUM_SECTORS) {
        return false;
    }
    memcpy(buf, medium + lba * SECTOR, (size_t)count * SECTOR);
    return true;
}

/* one ustar header, written the way tar writes one */
static void put_header(uint8_t *at, const char *prefix, const char *name,
                       uint64_t size, unsigned mode, char type)
{
    struct tar_header h;
    memset(&h, 0, sizeof h);
    strncpy(h.name, name, sizeof h.name - 1);
    if (prefix != NULL) {
        strncpy(h.prefix, prefix, sizeof h.prefix - 1);
    }
    snprintf(h.mode, sizeof h.mode, "%07o", mode);
    snprintf(h.size, sizeof h.size, "%011llo", (unsigned long long)size);
    memcpy(h.magic, "ustar", 5);
    h.version[0] = '0';
    h.version[1] = '0';
    h.typeflag = type;

    /*
     * the checksum, which this reader does not look at and a tar that
     * did not write one would still be a tar that lies to everything
     * else that reads it
     */
    memset(h.checksum, ' ', sizeof h.checksum);
    unsigned sum = 0;
    const uint8_t *raw = (const uint8_t *)&h;
    for (size_t i = 0; i < sizeof h; i++) {
        sum += raw[i];
    }
    snprintf(h.checksum, sizeof h.checksum, "%06o", sum);

    memcpy(at, &h, sizeof h);
}

/* an entry: header, contents, padded to a block */
static uint64_t put_file(uint8_t *archive, uint64_t at, const char *name,
                         const char *body, unsigned mode)
{
    uint64_t size = strlen(body);
    put_header(archive + at, NULL, name, size, mode, '0');
    memcpy(archive + at + USTAR_BLOCK, body, size);
    return at + USTAR_BLOCK + ((size + USTAR_BLOCK - 1) / USTAR_BLOCK)
           * USTAR_BLOCK;
}

/*
 * the file that crosses sector boundaries, so that reading part of one
 * is not the same question as reading all of a small one
 */
#define LONG_BYTES 4000
static char long_body[LONG_BYTES + 1];

static void make_long_body(void)
{
    for (int i = 0; i < LONG_BYTES; i++) {
        long_body[i] = (char)('a' + (i * 7 + i / 26) % 26);
    }
    long_body[LONG_BYTES] = '\0';
}

/*
 * build a medium. `magic` and `size_over` are the knobs the refusals
 * below need: a table nobody wrote, and a table that claims more than
 * was set aside for it
 */
static uint64_t archive_lba = 64;

static void make_medium(uint64_t magic, int64_t size_adjust)
{
    memset(medium, 0, MEDIUM_SECTORS * SECTOR);

    uint8_t *archive = medium + archive_lba * SECTOR;
    uint64_t at = 0;
    at = put_file(archive, at, "GNUmakefile", "all: the machine\n", 0644);
    at = put_file(archive, at, "kernel/main.c", "int kmain(void);\n", 0644);
    at = put_file(archive, at, "kernel/fs/vfs.c", "one namespace\n", 0644);
    at = put_file(archive, at, "tools/boottest.sh", "#!/bin/sh\n", 0755);
    at = put_file(archive, at, "userland/hello.c", long_body, 0644);

    /*
     * a directory record, which tar writes when it is given a directory
     * and which this archive never has, but a reader that hands one
     * back as an empty file would put a file called "boot/" in every
     * listing, and the installer would try to create it
     */
    put_header(archive + at, NULL, "boot/", 0, 0755, '5');
    at += USTAR_BLOCK;

    at = put_file(archive, at, "boot/philemon.c", "the loader\n", 0644);

    /* a name too long for the name field, split the way ustar splits one. */
    const char *deep = "and the deepest file of all\n";
    put_header(archive + at, "kernel/very/deeply/nested/directory",
               "with-a-name-that-does-not-fit-in-one-hundred-bytes.c",
               strlen(deep), 0644, '0');
    memcpy(archive + at + USTAR_BLOCK, deep, strlen(deep));
    at += USTAR_BLOCK * 2;

    /* the two zero blocks that end an archive */
    uint64_t size = at + USTAR_BLOCK * 2;

    struct ph_table t;
    memset(&t, 0, sizeof t);
    t.magic = magic;
    t.handoff_lba = 33;
    t.handoff_sectors = 8;
    t.kernel_lba = 41;
    t.kernel_sectors = 16;
    t.kernel_size = 16 * SECTOR;
    t.ramdisk_lba = 57;
    t.ramdisk_sectors = 7;
    t.ramdisk_size = 7 * SECTOR;
    t.source_lba = archive_lba;
    t.source_sectors = (size + SECTOR - 1) / SECTOR;
    t.source_size = (uint64_t)((int64_t)size + size_adjust);
    memcpy(medium + PH_TABLE_LBA * SECTOR, &t, sizeof t);
}

static bool body_of(const char *name, char *out, size_t max)
{
    struct source_file f;
    if (!source_open(name, &f)) {
        return false;
    }
    if (f.size >= max) {
        return false;
    }
    int64_t n = source_read_at(f.at, f.size, 0, out, f.size);
    if (n != (int64_t)f.size) {
        return false;
    }
    out[f.size] = '\0';
    return true;
}

int main(void)
{
    medium = malloc(MEDIUM_SECTORS * SECTOR);
    make_long_body();

    char body[LONG_BYTES + 64];
    struct source_file f;



    make_medium(0, 0);              /* no magic: not a boot medium */
    source_mount(medium_read, NULL);
    CHECK(!source_present(), "a medium with no philemon table carries nothing");
    CHECK(!source_open("GNUmakefile", &f),
          "and nothing can be opened out of it");
    CHECK(source_count() == 0, "and it holds no files");

    make_medium(PHILEMON_MAGIC, 0);
    medium_broken = true;
    source_mount(medium_read, NULL);
    CHECK(!source_present(), "a drive that will not answer carries nothing");
    medium_broken = false;

    /* a table saying the archive is bigger than the room set aside for it. */
    make_medium(PHILEMON_MAGIC, SECTOR);
    source_mount(medium_read, NULL);
    CHECK(!source_present(),
          "a table claiming more bytes than sectors is refused");

    /*
     * an image with no source in it: the fields are zero, and that is an
     * answer rather than a fault.
     */
    make_medium(PHILEMON_MAGIC, 0);
    {
        struct ph_table t;
        memcpy(&t, medium + PH_TABLE_LBA * SECTOR, sizeof t);
        t.source_lba = t.source_sectors = t.source_size = 0;
        memcpy(medium + PH_TABLE_LBA * SECTOR, &t, sizeof t);
    }
    source_mount(medium_read, NULL);
    CHECK(!source_present(), "an image built without a source tree says so");



    make_medium(PHILEMON_MAGIC, 0);
    source_mount(medium_read, NULL);

    CHECK(source_present(), "a medium with an archive on it carries one");
    CHECK(source_lba() == archive_lba, "and says where it is");
    CHECK(source_count() == 7,
          "seven files, and the directory record is not one of them");

    /*
     * the order is the order the archive holds, which is the order the
     * build sorted them into
     */
    CHECK(source_stat(0, &f) && strcmp(f.name, "GNUmakefile") == 0,
          "the first file is the first file");
    CHECK(source_stat(1, &f) && strcmp(f.name, "kernel/main.c") == 0,
          "and the second is the second");
    CHECK(!source_stat(7, &f), "and there is no eighth");

    CHECK(source_open("kernel/fs/vfs.c", &f), "a name is found");
    CHECK(f.size == strlen("one namespace\n"), "with its size");
    CHECK(f.mode == 0644, "and the mode tar recorded");
    CHECK(f.at % SECTOR == 0,
          "and its bytes start on a sector, which is what makes reading "
          "them arithmetic");

    CHECK(source_open("tools/boottest.sh", &f) && f.mode == 0755,
          "an executable file keeps its bit");

    CHECK(!source_open("kernel/fs/ext4.c", &f),
          "a name that is not in there is not found");
    CHECK(!source_open("boot/", &f),
          "and the directory record cannot be opened either");
    CHECK(!source_open("", &f), "nor can nothing at all");

    CHECK(body_of("GNUmakefile", body, sizeof body)
          && strcmp(body, "all: the machine\n") == 0,
          "a small file reads back exactly");
    CHECK(body_of("boot/philemon.c", body, sizeof body)
          && strcmp(body, "the loader\n") == 0,
          "and so does the one after the directory record");
    CHECK(body_of("kernel/very/deeply/nested/directory/"
                  "with-a-name-that-does-not-fit-in-one-hundred-bytes.c",
                  body, sizeof body)
          && strcmp(body, "and the deepest file of all\n") == 0,
          "a name split across the prefix field is put back together");



    CHECK(source_open("userland/hello.c", &f) && f.size == LONG_BYTES,
          "the long file is as long as it was written");

    CHECK(body_of("userland/hello.c", body, sizeof body)
          && memcmp(body, long_body, LONG_BYTES) == 0,
          "and reads back whole, across eight sectors");

    /*
     * every offset and length that is awkward: starting mid-sector,
     * ending mid-sector, a length that spans exactly one boundary, and
     * the last byte of the file
     */
    static const struct { uint64_t off, len; } bites[] = {
        { 0, 1 }, { 1, 1 }, { 0, 511 }, { 0, 512 }, { 0, 513 },
        { 511, 1 }, { 511, 2 }, { 511, 1026 }, { 512, 512 },
        { 1000, 2000 }, { LONG_BYTES - 1, 1 }, { 3, LONG_BYTES - 3 },
    };
    for (size_t i = 0; i < sizeof bites / sizeof bites[0]; i++) {
        char got[LONG_BYTES + 8];
        int64_t n = source_read_at(f.at, f.size, bites[i].off, got,
                                   bites[i].len);
        char msg[96];
        snprintf(msg, sizeof msg, "%llu bytes at %llu read back exactly",
                 (unsigned long long)bites[i].len,
                 (unsigned long long)bites[i].off);
        CHECK(n == (int64_t)bites[i].len
              && memcmp(got, long_body + bites[i].off, bites[i].len) == 0,
              msg);
    }

    /*
     * and the ends of it. a read past the end is nothing rather than an
     * error, that is what every other read in this kernel does when a
     * file runs out, and one that starts inside and runs past stops
     * where the file does
     */
    CHECK(source_read_at(f.at, f.size, LONG_BYTES, body, 16) == 0,
          "a read starting past the end reads nothing");
    CHECK(source_read_at(f.at, f.size, LONG_BYTES - 10, body, 100) == 10,
          "and one that runs off the end stops at it");

    /*
     * listing a directory and then opening what was listed is what the
     * installer does, three hundred and eighty-odd times. done the
     * obvious way that is a walk of the archive per file, off a drive,
     * and the whole point of remembering where the last one ended is
     * that it is not
     */
    reads = 0;
    for (size_t i = 0; source_stat(i, &f); i++) {
        struct source_file again;
        CHECK(source_open(f.name, &again) && again.at == f.at,
              "the file just listed is the file that opens");
    }
    CHECK(reads <= 16,
          "listing every file and opening each one costs about one read "
          "per file, not one walk per file");

    /* jumping about is allowed to cost the walk it would have cost */
    CHECK(source_stat(4, &f) && strcmp(f.name, "userland/hello.c") == 0,
          "and an index out of order still lands on the right file");
    CHECK(source_stat(1, &f) && strcmp(f.name, "kernel/main.c") == 0,
          "and going backwards does too");

    /* the last file's contents run past the end of the region. */
    make_medium(PHILEMON_MAGIC, 0);
    {
        struct ph_table t;
        memcpy(&t, medium + PH_TABLE_LBA * SECTOR, sizeof t);
        t.source_size -= USTAR_BLOCK * 3;   /* past the zero blocks, into
                                             * the last file */
        memcpy(medium + PH_TABLE_LBA * SECTOR, &t, sizeof t);
    }
    source_mount(medium_read, NULL);
    CHECK(source_present(), "a half-written archive is still an archive");
    CHECK(source_count() == 6, "and stops at the last file that is all there");
    CHECK(!source_open("kernel/very/deeply/nested/directory/"
                       "with-a-name-that-does-not-fit-in-one-hundred-bytes.c",
                       &f),
          "the file that runs off the end is not handed back");
    CHECK(source_open("boot/philemon.c", &f),
          "and everything before it still is");



    make_medium(PHILEMON_MAGIC, 0);
    source_mount(medium_read, NULL);

    uint8_t digest[20], expected[20];
    CHECK(source_digest(digest), "the whole archive can be hashed");
    sha1_of(medium + archive_lba * SECTOR, source_bytes(), expected);
    CHECK(memcmp(digest, expected, 20) == 0,
          "and it is the hash of exactly the bytes of the archive, no "
          "padding and no short read");

    medium_broken = true;
    CHECK(!source_digest(digest),
          "a drive that dies mid-hash gives no answer rather than a wrong one");
    medium_broken = false;

    free(medium);
    if (failures > 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all source checks passed\n");
    return 0;
}

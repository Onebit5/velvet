// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/source.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the source tree, still on the medium it booted from.
 */

#include "fs/source.h"
#include "fs/ustar.h"
#include "lib/hash.h"
#include "lib/string.h"
#include "philemon.h"

#define SECTOR 512

/* the medium, as a function that reads sectors off it. */
static bool (*medium)(void *, uint64_t, uint32_t, void *);
static void *medium_ctx;

static uint64_t region_lba;
static uint64_t region_size;
static uint64_t region_sectors;

/* how many files there are, once anybody has asked. */
static size_t counted;
static bool have_counted;

/*
 * everything below walks the archive from the front, which is what a
 * tar is: three hundred and eighty-four headers, each one saying how
 * far the next one is. that is fine for one lookup and quadratic for
 * the two things that actually happen, listing a directory, and the
 * installer listing every file and then opening each one it listed.
 * three hundred and eighty-four squared reads of a sector each, off a
 * drive, in the middle of an install.
 *
 * so the last file found is kept. asking for the next index continues
 * from where the last one ended, and asking to open the name that was
 * just listed answers without touching the drive at all. both are
 * exactly what the callers do, and a caller that does something else
 * gets the walk it would have got anyway.
 */
static struct {
    bool valid;
    size_t index;
    uint64_t next;          /* where the header after it starts */
    struct source_file file;
} last;

static void forget(void)
{
    last.valid = false;
    have_counted = false;
    counted = 0;
}



static bool read_sectors(uint64_t offset, uint32_t count, void *buf)
{
    if (medium == NULL) {
        return false;
    }
    /*
     * a length past the end of the region is a corrupt archive, not a
     * reason to go reading whatever the next partition holds
     */
    if (offset + count > region_sectors) {
        return false;
    }
    return medium(medium_ctx, region_lba + offset, count, buf);
}

/* the header at a byte offset into the archive, which is always a block boundary. */
static bool header_at(uint64_t offset, struct source_file *out,
                      uint64_t *next)
{
    if (offset + USTAR_BLOCK > region_size) {
        return false;
    }

    uint8_t block[USTAR_BLOCK];
    if (!read_sectors(offset / SECTOR, 1, block)) {
        return false;
    }

    const struct tar_header *h = (const struct tar_header *)block;
    if (h->name[0] == '\0' || !ustar_valid(h)) {
        return false;       /* the zero blocks that end an archive, or
                             * something the kernel should stop reading */
    }

    /*
     * a name is the prefix, a slash, and the name, and the prefix is
     * empty for every path in this tree, since none of them come near a
     * hundred bytes. it is honoured anyway: a reader that quietly drops
     * the prefix hands back a file under the wrong name, which is worse
     * than refusing it
     */
    size_t n = 0;
    if (h->prefix[0] != '\0') {
        while (n < sizeof h->prefix && h->prefix[n] != '\0'
               && n < SOURCE_NAME_MAX - 2) {
            out->name[n] = h->prefix[n];
            n++;
        }
        out->name[n++] = '/';
    }
    for (size_t i = 0; i < sizeof h->name && h->name[i] != '\0'
                       && n < SOURCE_NAME_MAX - 1; i++) {
        out->name[n++] = h->name[i];
    }
    out->name[n] = '\0';

    out->size = ustar_octal(h->size, sizeof h->size);
    out->mode = (uint32_t)ustar_octal(h->mode, sizeof h->mode);
    out->at   = region_lba * SECTOR + offset + USTAR_BLOCK;

    /*
     * a file whose contents run off the end of the region is an archive
     * that did not all get written. say nothing rather than hand back a
     * size that would read into whatever follows
     */
    if (offset + USTAR_BLOCK + out->size > region_size) {
        return false;
    }

    *next = ustar_next(offset, h);
    return true;
}

/* the same, skipping anything that is not an ordinary file. */
static bool file_at(uint64_t offset, struct source_file *out,
                    uint64_t *next)
{
    while (header_at(offset, out, next)) {
        size_t n = strlen(out->name);
        if (n > 0 && out->name[n - 1] != '/') {
            return true;
        }
        offset = *next;
    }
    return false;
}



void source_mount(bool (*read)(void *, uint64_t, uint32_t, void *),
                  void *ctx)
{
    medium = read;
    medium_ctx = ctx;
    region_lba = region_size = region_sectors = 0;
    forget();

    if (read == NULL) {
        return;
    }

    uint8_t sector[SECTOR];
    if (!read(ctx, PH_TABLE_LBA, 1, sector)) {
        medium = NULL;
        return;
    }

    struct ph_table t;
    memcpy(&t, sector, sizeof t);
    if (t.magic != PHILEMON_MAGIC || t.source_size == 0) {
        /* not a boot medium, or one built before there was a source tree to put on it. */
        medium = NULL;
        return;
    }

    /*
     * the table has to agree with itself before any of it is used: a
     * size claiming more than the sectors set aside for it would have
     * every read below trusting a number nothing wrote
     */
    if (t.source_size > t.source_sectors * SECTOR) {
        medium = NULL;
        return;
    }

    region_lba = t.source_lba;
    region_size = t.source_size;
    region_sectors = t.source_sectors;
}

bool     source_present(void)
{
    return medium != NULL && region_size > 0;
}
uint64_t source_lba(void)
{
    return region_lba;
}
uint64_t source_bytes(void)
{
    return region_size;
}

size_t source_count(void)
{
    if (have_counted) {
        return counted;
    }

    size_t n = 0;
    struct source_file f;
    uint64_t next;
    for (uint64_t off = 0; file_at(off, &f, &next); off = next) {
        n++;
    }

    counted = n;
    have_counted = true;
    return n;
}

bool source_stat(size_t index, struct source_file *out)
{
    uint64_t off = 0;
    size_t i = 0;

    if (last.valid && index == last.index) {
        *out = last.file;
        return true;
    }
    if (last.valid && index == last.index + 1) {
        off = last.next;
        i = index;
    }

    struct source_file f;
    uint64_t next;
    while (file_at(off, &f, &next)) {
        if (i == index) {
            last.valid = true;
            last.index = i;
            last.next = next;
            last.file = f;
            *out = f;
            return true;
        }
        i++;
        off = next;
    }
    return false;
}

bool source_open(const char *name, struct source_file *out)
{
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    /*
     * the one that was just listed, which is the one about to be opened
     * nine times out of ten
     */
    if (last.valid && strcmp(last.file.name, name) == 0) {
        *out = last.file;
        return true;
    }

    struct source_file f;
    for (size_t i = 0; source_stat(i, &f); i++) {
        if (strcmp(f.name, name) == 0) {
            *out = f;
            return true;
        }
    }
    return false;
}

/*
 * a file starts at a sector boundary and ends wherever it ends, so a
 * read is up to three: the part of a sector before the aligned middle,
 * the middle straight into the caller's buffer, and the tail. doing it
 * a sector at a time through a bounce buffer would be shorter and would
 * turn the installer's four-kilobyte reads into eight transfers each
 */
int64_t source_read_at(uint64_t at, uint64_t size, uint64_t offset,
                       void *buf, uint64_t len)
{
    if (!source_present()) {
        return -1;
    }
    if (offset >= size) {
        return 0;
    }
    if (offset + len > size) {
        len = size - offset;
    }

    uint8_t *out = buf;
    uint64_t done = 0;
    uint64_t base = at - region_lba * SECTOR;   /* back into the archive */

    while (done < len) {
        uint64_t here = base + offset + done;
        uint64_t sector = here / SECTOR;
        uint64_t within = here % SECTOR;
        uint64_t left = len - done;

        if (within == 0 && left >= SECTOR) {
            uint32_t run = (uint32_t)(left / SECTOR);
            if (run > 64) {
                run = 64;       /* somebody else's turn at the drive */
            }
            if (!read_sectors(sector, run, out + done)) {
                return done > 0 ? (int64_t)done : -1;
            }
            done += (uint64_t)run * SECTOR;
            continue;
        }

        uint8_t block[SECTOR];
        if (!read_sectors(sector, 1, block)) {
            return done > 0 ? (int64_t)done : -1;
        }
        uint64_t take = SECTOR - within;
        if (take > left) {
            take = left;
        }
        memcpy(out + done, block + within, take);
        done += take;
    }

    return (int64_t)done;
}

bool source_digest(uint8_t out[20])
{
    if (!source_present()) {
        return false;
    }

    struct sha1 s;
    sha1_init(&s);

    /*
     * eight sectors at a time, which is what install.c reads with and
     * for the same reason: a buffer big enough to be worth the trip and
     * small enough to sit on a kernel stack
     */
    uint8_t buf[SECTOR * 8];
    for (uint64_t done = 0; done < region_size; ) {
        uint64_t left = region_size - done;
        uint32_t run = 8;
        if (left < (uint64_t)run * SECTOR) {
            run = (uint32_t)((left + SECTOR - 1) / SECTOR);
        }
        if (!read_sectors(done / SECTOR, run, buf)) {
            return false;
        }
        uint64_t took = (uint64_t)run * SECTOR;
        if (took > left) {
            took = left;        /* the archive ends inside the last sector */
        }
        sha1_update(&s, buf, took);
        done += took;
    }

    sha1_final(&s, out);
    return true;
}

#ifndef VELVETOS_HOSTED

#include "fs/disk.h"
#include "lib/kprintf.h"

/*
 * take whichever drive philemon booted from, which is the one his table
 * is on, which is the one the archive is on. all three are the same
 * drive by construction: the source was written into the image beside
 * the kernel that is running
 */
void source_init(void)
{
    source_mount(disk_system_read, NULL);

    if (!source_present()) {
        kprintf("source     : none in this image, this machine cannot "
                "show you what it is made of\n");
        return;
    }

    kprintf("source     : %zu files, %lu KiB at lba %lu, read from the "
            "medium and never loaded\n",
            source_count(), source_bytes() / 1024, source_lba());
}

#endif

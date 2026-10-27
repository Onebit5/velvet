// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_bcache.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the block cache.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include <velvet/bcache.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)



/*
 * comfortably more blocks than the cache holds, so that reading
 * through it really does force eviction, which is where a dirty block
 * gets written or quietly lost
 */
#define DISK_SECTORS 4096
static uint8_t disk[DISK_SECTORS * BCACHE_SECTOR];

/*
 * and a second drive, which the cache must never mistake for the first.
 * what it holds differs from the first in every byte of every sector
 */
#define OTHER_SALT 128
static uint8_t s_arr_other[DISK_SECTORS * BCACHE_SECTOR];

static uint64_t reads_of_drive, writes_of_drive;
static bool drive_refuses;

/* the ctx is the drive, and a drive here is its bytes. */
static bool drive_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    const uint8_t *p_bytes = ctx;
    if (drive_refuses) return false;
    if (lba + count > DISK_SECTORS) return false;
    reads_of_drive++;
    memcpy(buf, &p_bytes[lba * BCACHE_SECTOR], count * BCACHE_SECTOR);
    return true;
}

static bool drive_write(void *ctx, uint64_t lba, uint32_t count,
                        const void *buf)
{
    uint8_t *p_bytes = ctx;
    if (drive_refuses) return false;
    if (lba + count > DISK_SECTORS) return false;
    writes_of_drive++;
    memcpy(&p_bytes[lba * BCACHE_SECTOR], buf, count * BCACHE_SECTOR);
    return true;
}

/*
 * what a sector should hold, so every byte can be checked rather than
 * the first one, a cache that reads the right block and the wrong
 * offset gets the first byte right surprisingly often
 */
static void fill(uint8_t *p, uint64_t lba, uint8_t salt)
{
    for (uint32_t i = 0; i < BCACHE_SECTOR; i++) {
        p[i] = (uint8_t)(lba * 7 + i * 3 + salt);
    }
}

static bool sector_is(const uint8_t *p, uint64_t lba, uint8_t salt)
{
    uint8_t want[BCACHE_SECTOR];
    fill(want, lba, salt);
    return memcmp(p, want, BCACHE_SECTOR) == 0;
}

static void reset_all(uint8_t salt)
{
    for (uint64_t s = 0; s < DISK_SECTORS; s++) {
        fill(&disk[s * BCACHE_SECTOR], s, salt);
        fill(&s_arr_other[s * BCACHE_SECTOR], s,
             (uint8_t)(salt + OTHER_SALT));
    }
    reads_of_drive = writes_of_drive = 0;
    drive_refuses = false;
    bcache_init(drive_read, drive_write);
}

int main(void)
{
    static uint8_t buf[64 * BCACHE_SECTOR];

    /*
     * an off-by-one here reads the right block and the wrong bytes,
     * which looks exactly like a filesystem bug
     */
    CHECK(bcache_block_of(0) == 0 && bcache_offset_of(0) == 0,
          "sector zero is the start of block zero");
    CHECK(bcache_block_of(BCACHE_BLOCK_SECTORS - 1) == 0,
          "and the last sector of a block is still in it");
    CHECK(bcache_offset_of(BCACHE_BLOCK_SECTORS - 1) == BCACHE_BLOCK_SECTORS - 1,
          "at the end of it");
    CHECK(bcache_block_of(BCACHE_BLOCK_SECTORS) == 1,
          "and the next one starts the next block");
    CHECK(bcache_offset_of(BCACHE_BLOCK_SECTORS) == 0, "at its start");



    reset_all(0);
    CHECK(bcache_read(disk, 3, 1, buf), "a sector reads");
    CHECK(sector_is(buf, 3, 0), "and holds what the disk holds");
    CHECK(reads_of_drive == 1, "having gone to the drive exactly once");

    /* the rest of that block is already here, and asking for it must not go anywhere. */
    uint64_t was = reads_of_drive;
    for (uint64_t s = 0; s < BCACHE_BLOCK_SECTORS; s++) {
        CHECK(bcache_read(disk, s, 1, buf), "every sector of that block reads");
        CHECK(sector_is(buf, s, 0), "correctly");
    }
    CHECK(reads_of_drive == was,
          "and not one of them went to the drive, one trip fetched all "
          "eight, which is the entire reason this exists");

    /* a read spanning two blocks has to take the right half of each */
    CHECK(bcache_read(disk, BCACHE_BLOCK_SECTORS - 2, 4, buf),
          "a read across a block boundary works");
    for (int i = 0; i < 4; i++) {
        CHECK(sector_is(buf + i * BCACHE_SECTOR,
                        BCACHE_BLOCK_SECTORS - 2 + i, 0),
              "with every sector of it in the right place");
    }

    /* and one spanning many */
    CHECK(bcache_read(disk, 5, 40, buf), "a long read works");
    for (int i = 0; i < 40; i++) {
        CHECK(sector_is(buf + i * BCACHE_SECTOR, 5 + i, 0),
              "every sector of a long read is right");
    }



    reset_all(0);
    uint8_t one[BCACHE_SECTOR];
    fill(one, 9, 42);

    CHECK(bcache_write(disk, 9, 1, one), "a sector writes");
    CHECK(writes_of_drive == 0,
          "and the drive has not heard about it, that is what write-back "
          "means, and it is the promise sync exists to keep");

    CHECK(bcache_read(disk, 9, 1, buf), "reading it back works");
    CHECK(sector_is(buf, 9, 42),
          "and gives what was written rather than what is on the disk");
    CHECK(!sector_is(&disk[9 * BCACHE_SECTOR], 9, 42),
          "which the disk still does not have");

    CHECK(bcache_dirty(), "and there is something to lose");

    CHECK(bcache_sync(), "sync works");
    CHECK(sector_is(&disk[9 * BCACHE_SECTOR], 9, 42),
          "and now the disk holds it too");
    CHECK(!bcache_dirty(), "with nothing left waiting");

    was = writes_of_drive;
    CHECK(bcache_sync(), "syncing again works");
    CHECK(writes_of_drive == was, "and writes nothing, because nothing changed");

    /* a partial write must not lose the rest of its block. */
    reset_all(0);
    fill(one, 17, 99);
    CHECK(bcache_write(disk, 17, 1, one), "one sector of a block is written");
    CHECK(bcache_sync(), "and synced");
    CHECK(sector_is(&disk[17 * BCACHE_SECTOR], 17, 99), "it landed");
    for (uint64_t s = 16; s < 24; s++) {
        if (s == 17) continue;
        CHECK(sector_is(&disk[s * BCACHE_SECTOR], s, 0),
              "and every other sector of that block is untouched");
    }

    /* a write covering a whole block need not read it first */
    reset_all(0);
    static uint8_t whole[BCACHE_BLOCK_BYTES];
    for (uint64_t i = 0; i < BCACHE_BLOCK_SECTORS; i++) {
        fill(whole + i * BCACHE_SECTOR, 40 + i, 7);
    }
    was = reads_of_drive;
    CHECK(bcache_write(disk, 40, BCACHE_BLOCK_SECTORS, whole),
          "a whole block writes");
    CHECK(reads_of_drive == was,
          "without reading it first, fetching bytes about to be thrown "
          "away is what makes writing a big file cost twice what it should");
    CHECK(bcache_sync(), "and syncs");
    for (uint64_t i = 0; i < BCACHE_BLOCK_SECTORS; i++) {
        CHECK(sector_is(&disk[(40 + i) * BCACHE_SECTOR], 40 + i, 7),
              "with all of it on the disk");
    }



    reset_all(0);
    fill(one, 1, 55);
    CHECK(bcache_write(disk, 1, 1, one), "a block is written");

    /* touch far more blocks than the cache holds, so the dirty one has to be thrown out. */
    for (uint64_t b = 100; b < 100 + BCACHE_BLOCKS * 2; b++) {
        CHECK(bcache_read(disk, b * BCACHE_BLOCK_SECTORS, 1, buf),
              "reading through more blocks than the cache holds works");
    }

    CHECK(sector_is(&disk[1 * BCACHE_SECTOR], 1, 55),
          "and the dirty block was written on its way out, rather than "
          "quietly lost");

    CHECK(bcache_read(disk, 1, 1, buf), "it can be read back");
    CHECK(sector_is(buf, 1, 55), "and still holds what was written");



    /*
     * two drives through one cache. the same sector number on each is
     * two different blocks, and a block goes back to the drive it came
     * from: a cache in front of two drives that writes one drive's block
     * onto the other has destroyed a filesystem nobody was using
     */
    reset_all(0);
    CHECK(bcache_read(disk, 5, 1, buf), "a sector of the first drive reads");
    CHECK(sector_is(buf, 5, 0), "and is the first drive's");
    CHECK(bcache_read(s_arr_other, 5, 1, buf),
          "the same sector of the second drive reads");
    CHECK(sector_is(buf, 5, OTHER_SALT),
          "and is the second drive's, not the block already held for the "
          "first");

    fill(one, 5, 11);
    CHECK(bcache_write(s_arr_other, 5, 1, one),
          "a sector of the second drive is written");
    CHECK(bcache_read(disk, 5, 1, buf), "the first drive's still reads");
    CHECK(sector_is(buf, 5, 0), "and still holds what it held");
    CHECK(bcache_sync(), "both sync");
    CHECK(sector_is(&s_arr_other[5 * BCACHE_SECTOR], 5, 11),
          "the write landed on the drive it was made to");
    CHECK(sector_is(&disk[5 * BCACHE_SECTOR], 5, 0),
          "and not on the other one");

    fill(one, 2, 77);
    CHECK(bcache_write(disk, 2, 1, one), "a block of the first drive is dirty");
    for (uint64_t b = 100; b < 100 + BCACHE_BLOCKS * 2; b++) {
        CHECK(bcache_read(s_arr_other, b * BCACHE_BLOCK_SECTORS, 1, buf),
              "reading the second drive through more blocks than the cache "
              "holds works");
    }
    CHECK(sector_is(&disk[2 * BCACHE_SECTOR], 2, 77),
          "and the dirty block, pushed out by the other drive's reads, went "
          "back to its own drive");
    CHECK(sector_is(&s_arr_other[2 * BCACHE_SECTOR], 2, OTHER_SALT),
          "leaving the other drive's sector of that number alone");



    reset_all(0);
    drive_refuses = true;
    CHECK(!bcache_read(disk, 300, 1, buf),
          "a read the drive refuses is refused rather than answered with "
          "whatever was in the slot");
    drive_refuses = false;
    CHECK(bcache_read(disk, 300, 1, buf), "and works once it stops refusing");
    CHECK(sector_is(buf, 300, 0), "with the right bytes");

    fill(one, 301, 3);
    bcache_write(disk, 301, 1, one);
    drive_refuses = true;
    CHECK(!bcache_sync(), "a sync the drive refuses says so");
    CHECK(bcache_dirty(), "and keeps the block, since it is still the only "
                          "copy there is");
    drive_refuses = false;
    CHECK(bcache_sync(), "and it goes out once the drive comes back");
    CHECK(sector_is(&disk[301 * BCACHE_SECTOR], 301, 3), "with the bytes");

    /*
     * the only assertion that really matters: for any sequence of reads
     * and writes, to either drive, the cache answers exactly what the
     * disks would have. a shadow copy of each is kept alongside and
     * every read checked against it
     */
    {
        reset_all(0);
        static uint8_t shadow[2][DISK_SECTORS * BCACHE_SECTOR];
        uint8_t *const arr_p_drives[2] = { disk, s_arr_other };
        memcpy(shadow[0], disk, sizeof shadow[0]);
        memcpy(shadow[1], s_arr_other, sizeof shadow[1]);

        unsigned seed = 987654321;
        #define NEXT() (seed = seed * 1103515245u + 12345u, (seed >> 16) & 0x7fff)

        bool ok = true;
        for (int round = 0; round < 200000 && ok; round++) {
            size_t sz_which = NEXT() % 2;
            uint8_t *p_drive = arr_p_drives[sz_which];
            uint8_t *p_shadow = shadow[sz_which];
            uint64_t lba = NEXT() % (DISK_SECTORS - 40);
            uint32_t count = NEXT() % 20 + 1;

            if (NEXT() % 3 == 0) {
                for (uint32_t i = 0; i < count; i++) {
                    fill(buf + i * BCACHE_SECTOR, lba + i, (uint8_t)round);
                }
                if (!bcache_write(p_drive, lba, count, buf)) {
                    ok = false;
                    break;
                }
                memcpy(&p_shadow[lba * BCACHE_SECTOR], buf,
                       count * BCACHE_SECTOR);
            } else {
                if (!bcache_read(p_drive, lba, count, buf)) {
                    ok = false;
                    break;
                }
                if (memcmp(buf, &p_shadow[lba * BCACHE_SECTOR],
                           count * BCACHE_SECTOR) != 0) {
                    ok = false;
                    break;
                }
            }
        }
        CHECK(ok, "two hundred thousand random reads and writes across two "
                  "drives agree with two real disks, every byte of them");

        CHECK(bcache_sync(), "and it all syncs");
        CHECK(memcmp(disk, shadow[0], sizeof shadow[0]) == 0
              && memcmp(s_arr_other, shadow[1], sizeof shadow[1]) == 0,
              "leaving each disk holding exactly what was written to it, "
              "which is the only promise any of this makes");
        #undef NEXT
    }


    {
        reset_all(0);
        struct bcache_stats st;

        bcache_read(disk, 0, 1, buf);
        bcache_read(disk, 1, 1, buf);
        bcache_read(disk, 2, 1, buf);
        bcache_get_stats(&st);
        CHECK(st.misses == 1, "one trip to the drive");
        CHECK(st.hits == 2, "and two answered without it");
        CHECK(st.held == 1, "holding one block");
        CHECK(st.dirty == 0, "none of it dirty");

        fill(one, 0, 1);
        bcache_write(disk, 0, 1, one);
        bcache_get_stats(&st);
        CHECK(st.writes == 1 && st.dirty == 1, "and a write makes it dirty");
        CHECK(st.writebacks == 0, "with nothing written back yet");

        bcache_sync();
        bcache_get_stats(&st);
        CHECK(st.writebacks == 1 && st.dirty == 0, "until sync");
    }

    if (failures == 0) printf("all good\n");
    return failures;
}

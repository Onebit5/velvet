// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_mkfs.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for the formatter.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "fs/mkfs.h"
#include "fs/ext4.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* a disk that is a lump of memory */
static uint8_t *image;
static uint64_t image_sectors;

static bool image_write(void *ctx, uint64_t lba, uint32_t count,
                        const void *buf)
{
    (void)ctx;
    if (lba + count > image_sectors) {
        return false;       /* off the end, which the formatter must not do */
    }
    memcpy(image + lba * 512, buf, (size_t)count * 512);
    return true;
}

static bool image_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (lba + count > image_sectors) {
        return false;
    }
    memcpy(buf, image + lba * 512, (size_t)count * 512);
    return true;
}

/* the driver writes too, and mounting does not, but it wants both */
static bool image_write_thunk(void *ctx, uint64_t lba, uint32_t count,
                              const void *buf)
{
    return image_write(ctx, lba, count, buf);
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/*
 * the journal, and only the journal, is big endian, so writing one
 * by hand needs a writer of its own, which is the point: nothing below
 * this line borrows a line of fs/jbd2.c
 */
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/*
 * the image is memory, so what has been written to it is on it already
 *, the promise the journal wants, and free here
 */
static bool host_sync(void *ctx)
{
    (void)ctx; return true;
}

static void make(uint64_t sectors)
{
    free(image);
    image_sectors = sectors;
    image = calloc(1, (size_t)sectors * 512);
    /*
     * poison it. a formatter that forgets to write a block would
     * otherwise be handed a conveniently zeroed one, and zero is a
     * plausible-looking value for far too many fields
     */
    memset(image, 0xa5, (size_t)sectors * 512);
}

int main(void)
{
    const char *error = NULL;
    struct mkfs_result r;



    make(MKFS_MIN_SECTORS);
    CHECK(!mkfs_ext4(image_write, NULL, 8, NULL, 0, &r, &error),
          "a disk of eight sectors is refused");
    CHECK(error != NULL, "and says why");



    const uint64_t SECTORS = 16384;         /* 8 MiB */
    make(SECTORS);
    error = NULL;
    CHECK(mkfs_ext4(image_write, NULL, SECTORS, "velvet", 1700000000, &r,
                    &error), "8 MiB formats");
    CHECK(error == NULL, "with nothing to report");
    CHECK(r.blocks == SECTORS / 2, "the block count is the disk in kibibytes");
    CHECK(r.groups == 1, "and 8 MiB is one block group");
    CHECK(r.free_blocks > 0 && r.free_blocks < r.blocks,
          "some of it is free and some of it is not");



    const uint8_t *sb = image + 1024;
    CHECK(rd16(sb + 56) == 0xef53, "the magic is where every reader looks");
    CHECK(rd32(sb + 4) == r.blocks, "the block count agrees with itself");
    CHECK(rd32(sb + 20) == 1,
          "first_data_block is 1, with 1 KiB blocks, block zero is the "
          "boot block and belongs to no group, and getting this wrong puts "
          "every bitmap one block from what it describes");
    CHECK(rd32(sb + 24) == 0, "1 KiB blocks");
    CHECK(rd32(sb + 32) == 8192, "8192 blocks per group, which is one bitmap");
    CHECK(rd32(sb + 40) == 512, "512 inodes per group");
    CHECK(rd32(sb + 76) == 1, "revision 1, or the fields after it mean nothing");
    CHECK(rd16(sb + 88) == 128, "128-byte inodes");
    CHECK(rd32(sb + 84) == 11, "and the first inode anybody may have is 11");
    CHECK(rd32(sb + 96) == (0x0002 | 0x0040),
          "FILETYPE and EXTENTS are the two features claimed, and claiming "
          "a feature that is not implemented is how a filesystem becomes "
          "unreadable");
    CHECK(rd32(sb + 92) == 0x0004,
          "a journal is claimed, and it is the only compatible feature, "
          "compatible because a reader that ignores it is still correct, "
          "only slower to recover");
    CHECK(rd32(sb + 224) == 8, "and inode 8 is where it says the log lives");
    CHECK(rd32(sb + 100) == 0,
          "and nothing else is, above all nothing with a checksum in it, "
          "since every write would then have to maintain a crc32c this "
          "kernel does not compute, and a filesystem whose checksums "
          "disagree with its contents is one fsck calls corrupt");
    CHECK(memcmp(sb + 120, "velvet", 6) == 0, "the label is written");
    CHECK(rd16(sb + 58) == 1, "and it is marked clean");

    /* the free counts have to be believable or a checker will say so */
    CHECK(rd32(sb + 12) == r.free_blocks, "free blocks agree with the plan");
    CHECK(rd32(sb + 16) == r.inodes - 10,
          "and ten inodes are spoken for by the format itself");



    const uint8_t *gd = image + 2 * 1024;
    uint32_t bbitmap = rd32(gd + 0), ibitmap = rd32(gd + 4), itable = rd32(gd + 8);

    /* the superblock, the descriptor table, the room the table may grow into, and then the bitmaps. */
    uint32_t reserved = rd16(sb + 206);
    CHECK(reserved > 0, "the descriptor table is given room to grow into");
    CHECK(bbitmap == 3 + reserved,
          "the block bitmap follows the superblock, the gdt, and the room "
          "the gdt may grow into");
    CHECK(ibitmap == bbitmap + 1, "then the inode bitmap");
    CHECK(itable == ibitmap + 1, "then the inode table");
    CHECK(rd16(gd + 16) == 1, "and the group holds exactly one directory");



    const uint8_t *root = image + itable * 1024 + 128;   /* inode 2 */
    CHECK((rd16(root + 0) & 0xf000) == 0x4000, "inode 2 is a directory");
    CHECK((rd16(root + 0) & 0777) == 0755, "and is 0755");
    CHECK(rd16(root + 26) == 2,
          "with two links, a directory points at itself through `.`, and "
          "one link would mean nothing points at it");
    CHECK(rd32(root + 4) == 1024, "one block of entries");
    CHECK(rd32(root + 28) == 2, "and i_blocks counts 512s, not blocks");

    /* where root's one block is, said the ext4 way. */
    CHECK((rd32(root + 32) & 0x00080000) != 0,
          "root is mapped by extents, and the inode says so");
    CHECK(rd16(root + 40) == 0xf30a, "with the magic every node carries");
    CHECK(rd16(root + 42) == 1, "one record in it");
    CHECK(rd16(root + 44) == 4, "and room in the inode for four");
    CHECK(rd16(root + 46) == 0, "a leaf, so the records are extents");
    CHECK(rd32(root + 52) == 0, "the run starts at logical block 0");
    CHECK(rd16(root + 56) == 1, "and is one block long");

    uint32_t rootblock = rd32(root + 60);
    CHECK(rootblock != 0, "root has a block");



    const uint8_t *dir = image + (uint64_t)rootblock * 1024;
    CHECK(rd32(dir + 0) == 2 && dir[6] == 1 && dir[8] == '.',
          "the first entry is `.`");
    CHECK(rd32(dir + 12) == 2 && dir[18] == 2
          && dir[20] == '.' && dir[21] == '.',
          "and the second is `..`");
    CHECK(rd16(dir + 4) + rd16(dir + 16) == 1024,
          "and the two lengths fill the block exactly, ext2 has no "
          "terminator, so the last record claiming the rest of the space "
          "*is* the end, and a reader walks into the next block without it");
    CHECK(dir[7] == 2 && dir[19] == 2,
          "both are marked as directories, since FILETYPE was claimed");



    bool poison = false;
    for (uint64_t b = 0; b <= rootblock; b++) {
        const uint8_t *p = image + b * 1024;
        bool all_poison = true;
        for (int i = 0; i < 1024; i++) {
            if (p[i] != 0xa5) { all_poison = false; break; }
        }
        if (all_poison) { poison = true; break; }
    }
    CHECK(!poison,
          "every metadata block was actually written, the image starts "
          "poisoned rather than zeroed, so a block the formatter skipped "
          "cannot pass for an empty one");



    static struct ext4 fs;
    CHECK(ext4_mount(&fs, image_read, image_write_thunk, NULL),
          "the ext2 driver mounts what the formatter produced");

    struct ext4_file f;
    CHECK(ext4_lookup(&fs, "/", &f), "and finds a root directory in it");
    CHECK(f.is_dir, "which is a directory");

    /*
     * and it is empty. ext4_readdir skips `.` and `..` by contract,
     * nothing above the driver has any use for them, so a freshly
     * formatted root has no entries at all, and index 0 says so
     */
    struct ext4_file entry;
    CHECK(!ext4_readdir(&fs, 2, 0, &entry),
          "and it is empty, which a new filesystem had better be");



    const uint64_t BIG = 40960;             /* 20 MiB, which needs three */
    make(BIG);
    error = NULL;
    CHECK(mkfs_ext4(image_write, NULL, BIG, "big", 1700000000, &r, &error),
          "20 MiB formats");
    CHECK(r.groups == 3, "and comes out as three groups");

    sb = image + 1024;
    CHECK(rd32(sb + 4) == BIG / 2, "the block count is still the whole disk");
    CHECK(rd16(sb + 90) == 0, "the primary superblock says it is copy zero");

    /* every group carries a backup, because sparse_super is not claimed. */
    const uint8_t *backup = image + (uint64_t)(1 + 8192) * 1024;
    CHECK(rd16(backup + 56) == 0xef53, "group 1 carries a superblock copy");
    CHECK(rd16(backup + 90) == 1, "which knows which copy it is");
    CHECK(rd32(backup + 4) == rd32(sb + 4),
          "and agrees with the primary about how big the disk is");

    CHECK(ext4_mount(&fs, image_read, image_write_thunk, NULL),
          "and the driver mounts the three-group one too");
    CHECK(ext4_lookup(&fs, "/", &f) && f.is_dir, "root is still there");

    /*
     * the usual case, and the one where the bitmap has to lie in the
     * right direction: the bits past the end of the disk must read as
     * *used*, or something will hand one of them out
     */

    const uint64_t ODD = 30000;             /* not a multiple of anything */
    make(ODD);
    error = NULL;
    CHECK(mkfs_ext4(image_write, NULL, ODD, NULL, 0, &r, &error),
          "an awkward size formats");
    CHECK(r.blocks == ODD / 2, "and uses all of it");

    uint32_t last = r.groups - 1;
    uint64_t last_base = 1 + (uint64_t)last * 8192;
    uint32_t in_last = (uint32_t)(r.blocks - last_base);
    const uint8_t *lgd = image + 2 * 1024 + (uint64_t)last * 32;
    const uint8_t *bmap = image + (uint64_t)rd32(lgd + 0) * 1024;

    CHECK((bmap[in_last / 8] >> (in_last % 8)) & 1,
          "the first bit past the end of the disk reads as used");
    /*
     * a bitmap block is 1024 *bytes* and therefore 8192 bits, so the
     * last bit lives in byte 1023. indexing it by bit number is a
     * mistake this test made once and is exactly the mistake that puts a
     * real bitmap one block away from what it describes
     */
    CHECK((bmap[1023] >> 7) & 1,
          "and so does the very last bit of the bitmap, which describes a "
          "block that is not there at all");

    CHECK(ext4_mount(&fs, image_read, image_write_thunk, NULL)
          && ext4_lookup(&fs, "/", &f),
          "and it still mounts");

    /*
     * everything the journal does elsewhere is checked by round trip:
     * this driver writes a transaction, this driver replays it, and the
     * two agree because they are the same three hundred lines. that is
     * the arrangement mkfat.py was wrong inside of for a whole version.
     *
     * so here the log is written *by hand*, from the format rather than
     * from fs/jbd2.c, twelve bytes of header, a tag saying where one
     * block belongs, the block, and a commit block behind it, every
     * field big endian because the format says so. if the driver
     * replays that, it agrees with the specification and not merely
     * with itself.
     *
     * and the other half, which matters more: the same log without the
     * commit block must be replayed by *nothing*. a journal that
     * applies a transaction whose end never arrived is worse than no
     * journal, because it writes a change the machine never finished
     * making
     */
    {
        const uint32_t BLOCK = 1024;
        const uint32_t HOME = 3000;         /* free space, and far away */

        /*
         * where the log is, found the way a reader finds it: inode 8,
         * through the group descriptor and the inode table, and its
         * extent tree read out by hand
         */
        make(SECTORS);
        error = NULL;
        CHECK(mkfs_ext4(image_write, NULL, SECTORS, "log", 1700000000, &r,
                        &error), "a disk to hand-write a log onto");

        const uint8_t *gd = image + 2 * BLOCK;
        uint32_t itable = rd32(gd + 8);
        const uint8_t *jn = image + itable * BLOCK + 7 * 128;
        CHECK(rd16(jn + 40) == 0xf30a && rd16(jn + 42) == 1,
              "the journal inode is one extent, which is how it was laid "
              "down: contiguous, and never moved");
        uint32_t log = rd32(jn + 40 + 12 + 8);
        CHECK(log > 0 && log < r.blocks, "and it is somewhere on the disk");

        uint8_t *jsb = image + (size_t)log * BLOCK;
        uint32_t first = (uint32_t)((jsb[20] << 24) | (jsb[21] << 16)
                                  | (jsb[22] << 8) | jsb[23]);
        CHECK(first == 1, "block 0 of a log is its superblock and block 1 "
                          "is where transactions start");

        /* the transaction, written from the format. */
        static const uint8_t WANT[8] = { 'b','y',' ','h','a','n','d' };
        for (int missing_commit = 0; missing_commit < 2; missing_commit++) {
            memset(image + (size_t)HOME * BLOCK, 0, BLOCK);

            uint8_t *desc = image + (size_t)(log + first) * BLOCK;
            memset(desc, 0, BLOCK);
            put_be32(desc + 0, 0xc03b3998u);        /* the magic */
            put_be32(desc + 4, 1);                  /* a descriptor */
            put_be32(desc + 8, 1);                  /* sequence one */
            put_be32(desc + 12, HOME);              /* where it belongs */
            put_be32(desc + 16, 8);                 /* no csum, LAST_TAG */
            memset(desc + 20, 0, 16);               /* the uuid it follows */

            uint8_t *body = image + (size_t)(log + first + 1) * BLOCK;
            memset(body, 0, BLOCK);
            memcpy(body, WANT, sizeof WANT);

            uint8_t *commit = image + (size_t)(log + first + 2) * BLOCK;
            memset(commit, 0, BLOCK);
            if (!missing_commit) {
                put_be32(commit + 0, 0xc03b3998u);
                put_be32(commit + 4, 2);            /* a commit block */
                put_be32(commit + 8, 1);            /* of that sequence */
            }

            /*
             * and the log's own superblock, saying there is something in
             * it and where it begins
             */
            put_be32(jsb + 24, 1);                  /* s_sequence */
            put_be32(jsb + 28, first);              /* s_start */

            /*
             * and the filesystem saying so too, which is the sentence a
             * disk pulled out of a crashed machine carries
             */
            uint8_t *fsb = image + 1024;
            put_le32(fsb + 96, rd32(fsb + 96) | 0x0004u);

            static struct ext4 fs;
            CHECK(ext4_mount(&fs, image_read, image_write_thunk, NULL),
                  "a filesystem whose log has something in it mounts");
            ext4_set_sync(&fs, host_sync);
            CHECK(ext4_start_journal(&fs),
                  "and the journal starts, which is what replays it");

            const uint8_t *home = image + (size_t)HOME * BLOCK;
            if (missing_commit) {
                bool untouched = true;
                for (uint32_t i = 0; i < BLOCK; i++) {
                    if (home[i] != 0) { untouched = false; break; }
                }
                CHECK(untouched,
                      "a transaction with no commit block behind it is not "
                      "replayed, it is where the machine stopped, and "
                      "finishing it would be inventing what it meant to do");
            } else {
                CHECK(memcmp(home, WANT, sizeof WANT) == 0,
                      "a transaction written by hand from the format is "
                      "replayed, which means the driver agrees with jbd2 "
                      "and not merely with itself");
            }

            /* and closing it says so on the disk both ways round */
            CHECK(ext4_stop_journal(&fs), "the journal closes cleanly");
            CHECK(rd32(image + 1024 + 96) & 0x0004u ? false : true,
                  "and a closed journal stops asking to be recovered");
            CHECK(jsb[28] == 0 && jsb[29] == 0 && jsb[30] == 0
                  && jsb[31] == 0,
                  "with a start of zero, which is a log saying it holds "
                  "nothing");
        }
    }

    /*
     * `make test` runs tools/readext2.py over this, and that reader was
     * written from the on-disk layout rather than from either the
     * formatter or the driver. everything above this line is two
     * programs by one author agreeing with each other, which is exactly
     * the arrangement that let mkfat.py be wrong for a whole version
     */
    make(SECTORS);
    if (mkfs_ext4(image_write, NULL, SECTORS, "velvet", 1700000000, &r,
                  &error)) {
        FILE *f = fopen("bin/tests/mkfs.img", "wb");
        if (f != NULL) {
            fwrite(image, 1, (size_t)SECTORS * 512, f);
            fclose(f);
        } else {
            printf("FAIL: could not leave an image for readext2.py\n");
            failures++;
        }
    }

    if (failures == 0) printf("all good\n");
    free(image);
    return failures;
}

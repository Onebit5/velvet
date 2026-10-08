// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_resize.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * growing a filesystem, and refusing to shrink one that would lose
 * something.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

void panic(const char *fmt, ...)
{
    printf("PANIC: ");
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    printf("\n");
    exit(1);
}

#include "fs/ext4.h"
#include "fs/mkfs.h"
#include "fs/fsck.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* which is the whole situation. */

#define IMAGE_SECTORS   (64 * 1024 * 2)         /* 64 MiB */
#define IMAGE_BYTES     ((size_t)IMAGE_SECTORS * 512)
#define SMALL_SECTORS   (8 * 1024 * 2)          /* 8 MiB of filesystem */

static uint8_t *image;
static long writes_left = -1;

static bool img_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if ((lba + count) * 512 > IMAGE_BYTES) {
        return false;
    }
    memcpy(buf, image + lba * 512, (size_t)count * 512);
    return true;
}

static bool img_write(void *ctx, uint64_t lba, uint32_t count,
                      const void *buf)
{
    (void)ctx;
    if ((lba + count) * 512 > IMAGE_BYTES) {
        return false;
    }
    if (writes_left == 0) {
        return false;           /* the power went */
    }
    if (writes_left > 0) {
        writes_left--;
    }
    memcpy(image + lba * 512, buf, (size_t)count * 512);
    return true;
}

/*
 * the image is memory, so what has been written to it is on it already
 *, the promise the journal wants, and free here
 */
static bool host_sync(void *ctx)
{
    (void)ctx; return true;
}

static bool mount_rw(struct ext4 *fs)
{
    if (!ext4_mount(fs, img_read, img_write, NULL)) {
        return false;
    }
    ext4_set_sync(fs, host_sync);
    return ext4_start_journal(fs);
}

/* what fs/fsck.c makes of it, which is the opinion that counts */
static uint32_t problems(const char **first)
{
    uint32_t blocks, inodes;
    *first = NULL;
    if (!fsck_measure(img_read, NULL, &blocks, &inodes)) {
        *first = "there is no filesystem here at all";
        return 1;
    }
    size_t need = fsck_workspace_bytes(blocks, inodes);
    void *work = malloc(need);
    if (work == NULL) {
        *first = "out of memory";
        return 1;
    }
    struct fsck_report r;
    if (!fsck_run(img_read, NULL, NULL, work, need, &r)) {
        free(work);
        *first = "fsck could not read it";
        return 1;
    }
    for (int i = 0; i < FSCK_PROBLEMS; i++) {
        if (r.found[i] > 0) {
            *first = fsck_problem_name((enum fsck_problem)i);
            break;
        }
    }
    uint32_t total = r.total_found;
    free(work);
    return total;
}

/* a file of `n` bytes, written and read back. */
static bool put_file(struct ext4 *fs, const char *name, uint32_t n)
{
    struct ext4_file f;
    if (!ext4_create(fs, name, 0644, 0, 0, &f)) {
        return false;
    }
    static uint8_t lump[64 * 1024];
    for (uint32_t i = 0; i < n && i < sizeof lump; i++) {
        lump[i] = (uint8_t)((i * 7 + name[0]) & 0xff);
    }
    uint32_t done = 0;
    while (done < n) {
        uint32_t take = n - done;
        if (take > sizeof lump) {
            take = sizeof lump;
        }
        if (ext4_write(fs, &f, done, lump, take) != (int64_t)take) {
            return false;
        }
        done += take;
    }
    return true;
}

static bool file_reads_back(struct ext4 *fs, const char *name, uint32_t n)
{
    struct ext4_file f;
    if (!ext4_lookup(fs, name, &f) || f.size != n) {
        return false;
    }
    static uint8_t got[64 * 1024];
    uint32_t done = 0;
    while (done < n) {
        uint32_t take = n - done;
        if (take > sizeof got) {
            take = sizeof got;
        }
        if (ext4_read(fs, &f, done, got, take) != (int64_t)take) {
            return false;
        }
        for (uint32_t i = 0; i < take; i++) {
            if (got[i] != (uint8_t)(((done + i) % sizeof got) * 7 + name[0])) {
                return false;
            }
        }
        done += take;
    }
    return true;
}

static void format(uint64_t sectors)
{
    memset(image, 0, IMAGE_BYTES);
    struct mkfs_result mk;
    const char *error = NULL;
    if (!mkfs_ext4(img_write, NULL, sectors, "grow", 1700000000, &mk,
                   &error)) {
        printf("FAIL: could not format: %s\n", error);
        failures++;
    }
}

int main(void)
{
    image = malloc(IMAGE_BYTES);
    if (image == NULL) {
        printf("FAIL: cannot allocate an image\n");
        return 1;
    }

    const char *why = NULL;
    struct ext4 fs;
    struct ext4_resize r;
    const char *error = NULL;



    format(SMALL_SECTORS);
    CHECK(mount_rw(&fs), "the small filesystem mounts");

    uint64_t ceiling = ext4_resize_ceiling(&fs);
    CHECK(ceiling > fs.blocks_count,
          "a freshly formatted filesystem has room to grow, because the "
          "formatter reserved descriptor blocks for it");
    CHECK(ceiling >= IMAGE_SECTORS / 2,
          "and enough of it to reach the end of a disk eight times its "
          "size, which is the point of reserving any");

    CHECK(!ext4_resize(&fs, ceiling + 1, &r, &error),
          "growing past the ceiling is refused");
    CHECK(error != NULL, "and says the table is what stops it");



    CHECK(put_file(&fs, "before.bin", 300000), "a file goes in first");
    CHECK(ext4_mkdir(&fs, "sub", 0755, 0, 0), "and a directory");
    CHECK(put_file(&fs, "sub/small.txt", 900), "with something in it");

    uint32_t was_blocks = fs.blocks_count;
    uint32_t was_free = fs.free_blocks;
    uint32_t was_inodes = fs.inodes_count;



    error = NULL;
    CHECK(ext4_resize(&fs, IMAGE_SECTORS / 2, &r, &error),
          "it grows to fill the disk");
    CHECK(error == NULL, "with nothing to report");
    CHECK(r.blocks_before == was_blocks && r.blocks_after == IMAGE_SECTORS / 2,
          "and says what it was and what it is");
    CHECK(r.groups_after > r.groups_before, "which took more groups");
    CHECK(fs.free_blocks > was_free,
          "there is more free space than there was, which is the entire "
          "point of the exercise");
    CHECK(fs.inodes_count > was_inodes,
          "and more inodes, a group brings its own, and a disk eight "
          "times the size with the same thousand inodes would fill up "
          "having used a tenth of itself");

    CHECK(file_reads_back(&fs, "before.bin", 300000),
          "the file written before the resize reads back, every byte");
    CHECK(file_reads_back(&fs, "sub/small.txt", 900), "and the small one");

    uint32_t found = problems(&why);
    CHECK(found == 0, why != NULL ? why : "the checker finds nothing wrong");

    /*
     * a resize that produced a filesystem the checker likes and the
     * allocator cannot use would pass everything above this line. so
     * this writes past where the old filesystem ended, which can only
     * be satisfied out of a group that did not exist an hour ago
     */
    for (int i = 0; i < 40; i++) {
        char name[32];
        snprintf(name, sizeof name, "after%02d.bin", i);
        CHECK(put_file(&fs, name, 200000), "a file goes into the new space");
    }
    CHECK(fs.blocks_count - fs.free_blocks > was_blocks,
          "and there is now more stored here than the whole of the old "
          "filesystem could have held, which is the only way to be sure "
          "the new groups are being allocated out of");
    CHECK(file_reads_back(&fs, "after00.bin", 200000),
          "and the first of them reads back");
    CHECK(file_reads_back(&fs, "after39.bin", 200000), "and the last");
    CHECK(file_reads_back(&fs, "before.bin", 300000),
          "and the old file is still itself");

    found = problems(&why);
    CHECK(found == 0, why != NULL ? why : "and the checker still agrees");



    {
        struct ext4 again;
        CHECK(mount_rw(&again), "the grown filesystem mounts");
        CHECK(again.blocks_count == IMAGE_SECTORS / 2,
              "at its new size, which came off the disk rather than out "
              "of memory");
        CHECK(file_reads_back(&again, "after39.bin", 200000),
              "and everything in the new space is still there");
        CHECK(ext4_stop_journal(&again), "and it closes cleanly");
    }



    CHECK(mount_rw(&fs), "mounted again to shrink");
    error = NULL;
    CHECK(!ext4_resize(&fs, SMALL_SECTORS / 2, &r, &error),
          "shrinking back with files out there is refused");
    CHECK(error != NULL, "and says why");
    CHECK(r.blocks_in_way > 0,
          "and says how much is in the way rather than merely that "
          "something is");
    CHECK(fs.blocks_count == IMAGE_SECTORS / 2,
          "and the filesystem is exactly the size it was, a refusal "
          "that changed something would be the worst of both");
    CHECK(file_reads_back(&fs, "after39.bin", 200000),
          "with everything still readable");

    for (int i = 0; i < 40; i++) {
        char name[32];
        snprintf(name, sizeof name, "after%02d.bin", i);
        CHECK(ext4_unlink(&fs, name), "the new files are removed");
    }

    error = NULL;
    CHECK(ext4_resize(&fs, SMALL_SECTORS / 2, &r, &error),
          "and now the tail is empty, it shrinks");
    CHECK(error == NULL, "with nothing to report");
    CHECK(fs.blocks_count == SMALL_SECTORS / 2, "back to where it started");
    CHECK(fs.inodes_count == was_inodes,
          "and the inodes the extra groups brought went with them");
    CHECK(file_reads_back(&fs, "before.bin", 300000),
          "the file that was there before all of this is still every byte "
          "of itself");

    found = problems(&why);
    CHECK(found == 0, why != NULL ? why : "and the checker agrees again");

    /*
     * this is the claim that growing is *additive*: until the block
     * count moves, none of the new groups exist, so a resize interrupted
     * anywhere is a filesystem that was never resized. every stopping
     * point, not a sample of them
     */
    {
        struct ext4 counted;
        format(SMALL_SECTORS);
        CHECK(mount_rw(&counted), "a filesystem to interrupt");
        CHECK(put_file(&counted, "keep.bin", 120000), "with something in it");

        long total = 0;
        {
            /* how many writes a grow takes, by doing one */
            writes_left = -1;
            uint8_t *saved = malloc(IMAGE_BYTES);
            memcpy(saved, image, IMAGE_BYTES);
            long before = 0;
            writes_left = 1000000;
            error = NULL;
            CHECK(ext4_resize(&counted, IMAGE_SECTORS / 4, &r, &error),
                  "which grows once, to be counted");
            total = 1000000 - writes_left + before;
            memcpy(image, saved, IMAGE_BYTES);
            free(saved);
            writes_left = -1;
        }
        CHECK(total > 20, "and takes a useful number of writes");

        long bad = 0;
        long first_bad = -1;
        for (long n = 0; n <= total; n++) {
            format(SMALL_SECTORS);
            struct ext4 f2;
            if (!mount_rw(&f2) || !put_file(&f2, "keep.bin", 120000)) {
                bad++;
                continue;
            }
            writes_left = n;
            error = NULL;
            (void)ext4_resize(&f2, IMAGE_SECTORS / 4, &r, &error);
            writes_left = -1;

            /*
             * the power comes back, the machine boots, the journal is
             * replayed, and then the question is asked
             */
            struct ext4 after;
            if (ext4_mount(&after, img_read, img_write, NULL)) {
                ext4_set_sync(&after, host_sync);
                ext4_start_journal(&after);
            }
            const char *w = NULL;
            if (problems(&w) != 0 || !file_reads_back(&after, "keep.bin",
                                                      120000)) {
                if (first_bad < 0) {
                    first_bad = n;
                }
                bad++;
            }
        }
        if (bad > 0) {
            printf("FAIL: %ld of %ld stopping points during a grow left "
                   "damage\n", bad, total + 1);
            printf("      the first is after %ld write(s)\n", first_bad);
            failures++;
        } else {
            printf("  %ld stopping points during a grow, and every one of "
                   "them is a filesystem\n", total + 1);
        }
    }



    format(SMALL_SECTORS);
    if (mount_rw(&fs)) {
        put_file(&fs, "grown.bin", 250000);
        error = NULL;
        if (!ext4_resize(&fs, IMAGE_SECTORS / 2, &r, &error)) {
            printf("FAIL: could not grow the image to leave behind\n");
            failures++;
        }
        put_file(&fs, "after-the-grow.bin", 400000);
        ext4_stop_journal(&fs);

        FILE *out = fopen("bin/tests/resize.img", "wb");
        if (out != NULL) {
            fwrite(image, 1, IMAGE_BYTES, out);
            fclose(out);
        } else {
            printf("FAIL: could not leave an image for readext4.py\n");
            failures++;
        }
    }

    free(image);
    if (failures == 0) {
        printf("all good\n");
    }
    return failures == 0 ? 0 : 1;
}

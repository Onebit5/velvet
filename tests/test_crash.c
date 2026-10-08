// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_crash.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * pulling the plug, over and over, and seeing what is left.
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

/*
 * the image is a file (or memory), so what has been written to it
 * is on it already, the promise the journal needs, and free here
 */
static bool host_sync(void *ctx)
{
    (void)ctx; return true;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

/* an image in memory, and a counter. */

#define IMAGE_SECTORS   (16 * 1024 * 2)         /* 16 MiB */
#define IMAGE_BYTES     ((size_t)IMAGE_SECTORS * 512)

static uint8_t *image;
static long writes_left;
static long writes_done;

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
    writes_done++;
    memcpy(image + lba * 512, buf, (size_t)count * 512);
    return true;
}

static bool mkfs_write(void *ctx, uint64_t lba, uint32_t count,
                       const void *buf)
{
    (void)ctx;
    if ((lba + count) * 512 > IMAGE_BYTES) {
        return false;
    }
    memcpy(image + lba * 512, buf, (size_t)count * 512);
    return true;
}



static void do_work(struct ext4 *fs)
{
    struct ext4_file f;
    static char lump[6000];
    for (size_t i = 0; i < sizeof lump; i++) {
        lump[i] = (char)(i & 0xff);
    }

    /*
     * the four shapes of change a filesystem makes: something appears,
     * something grows, something is renamed, something goes away. each
     * of them touches a different set of metadata, and each has an order
     * that leaves a leak and an order that leaves a dangling pointer
     */
    if (ext4_create(fs, "one.txt", 0644, 0, 0, &f)) {
        ext4_write(fs, &f, 0, lump, sizeof lump);
    }
    if (ext4_mkdir(fs, "sub", 0755, 0, 0)) {
        if (ext4_create(fs, "sub/two.txt", 0644, 0, 0, &f)) {
            ext4_write(fs, &f, 0, lump, 400);
        }
    }
    ext4_symlink(fs, "link", "one.txt", 0, 0);
    ext4_rename(fs, "one.txt", "sub/moved.txt");
    ext4_unlink(fs, "sub/two.txt");
    ext4_rmdir(fs, "nothing-here");
}



/* the kinds of damage that have one right answer. */
static bool mendable(enum fsck_problem p)
{
    return p == FSCK_BLOCK_LEAKED
        || p == FSCK_INODE_ORPHANED
        || p == FSCK_LINKS_WRONG
        || p == FSCK_COUNTS_WRONG;
}

static const char *survey(long stop_after, bool *clean_after_mending)
{
    static char why[256];
    *clean_after_mending = false;

    /*
     * a fresh filesystem every time, so one run cannot leave the next
     * one something to trip over
     */
    memset(image, 0, IMAGE_BYTES);
    struct mkfs_result mk;
    const char *error = NULL;
    if (!mkfs_ext4(mkfs_write, NULL, IMAGE_SECTORS, "crash", 1700000000,
                   &mk, &error)) {
        snprintf(why, sizeof why, "could not format: %s", error);
        return why;
    }

    writes_left = stop_after;
    writes_done = 0;

    struct ext4 fs;
    if (ext4_mount(&fs, img_read, img_write, NULL)) {
        ext4_set_sync(&fs, host_sync);
        ext4_start_journal(&fs);
        do_work(&fs);
    }

    /*
     * the power comes back on before anything else happens, because
     * that is the only order there is: a machine with no power is not
     * running fsck either. every write from here is the next boot's
     */
    writes_left = -1;

    /* and the next boot's first act is recovery, which is what a journal is for. */
    {
        struct ext4 again;
        if (ext4_mount(&again, img_read, img_write, NULL)) {
            ext4_set_sync(&again, host_sync);
            ext4_start_journal(&again);
        }
    }

    /* now: what is on the disk, and can it be put right? */
    uint32_t blocks, inodes;
    if (!fsck_measure(img_read, NULL, &blocks, &inodes)) {
        snprintf(why, sizeof why, "no filesystem left at all");
        return why;
    }
    size_t need = fsck_workspace_bytes(blocks, inodes);
    void *work = malloc(need);
    if (work == NULL) {
        return "out of memory";
    }

    struct fsck_report r;
    if (!fsck_run(img_read, NULL, NULL, work, need, &r)) {
        free(work);
        snprintf(why, sizeof why, "fsck could not read it: %s",
                 r.stopped != NULL ? r.stopped : "?");
        return why;
    }

    /*
     * nothing at all is the bar, and the two kinds are still worth
     * telling apart in the message: damage fsck could mend means the
     * replay left work behind, and damage it could not means the replay
     * left the filesystem wrong
     */
    for (int i = 0; i < FSCK_PROBLEMS; i++) {
        if (r.found[i] > 0) {
            snprintf(why, sizeof why, "%u x %s%s", r.found[i],
                     fsck_problem_name((enum fsck_problem)i),
                     mendable((enum fsck_problem)i)
                         ? ", mendable, but the replay should have left "
                           "nothing" : "");
            free(work);
            return why;
        }
    }

    /*
     * and mending it has to actually work, a run that reports nothing
     * afterwards is the only proof that the first run understood what it
     * was looking at. with nothing to mend it is a run that changes
     * nothing, which is the other thing worth knowing
     */
    if (!fsck_run(img_read, img_write, NULL, work, need, &r)) {
        free(work);
        return "fsck could not mend it";
    }
    struct fsck_report again;
    if (!fsck_run(img_read, NULL, NULL, work, need, &again)) {
        free(work);
        return "fsck could not read what it had just mended";
    }
    free(work);

    if (again.total_found != 0) {
        for (int i = 0; i < FSCK_PROBLEMS; i++) {
            if (again.found[i] > 0) {
                snprintf(why, sizeof why, "%u x %s survived the mending",
                         again.found[i],
                         fsck_problem_name((enum fsck_problem)i));
                return why;
            }
        }
    }
    *clean_after_mending = true;
    return NULL;
}

int main(void)
{
    image = malloc(IMAGE_BYTES);
    if (image == NULL) {
        printf("FAIL: cannot allocate an image\n");
        return 1;
    }

    /*
     * one uninterrupted run first, to find out how many writes the work
     * takes, and to be sure the work itself is sound before anything
     * is interrupted
     */
    bool clean;
    const char *why = survey(-1, &clean);
    CHECK(why == NULL, why != NULL ? why : "an uninterrupted run is clean");
    long total = writes_done;
    CHECK(total > 20, "the work takes a useful number of writes");

    /* then every stopping point there is */
    long bad = 0;
    long first_bad = -1;
    static char first_why[256];

    for (long n = 0; n <= total; n++) {
        why = survey(n, &clean);
        if (why != NULL) {
            if (first_bad < 0) {
                first_bad = n;
                snprintf(first_why, sizeof first_why, "%s", why);
            }
            bad++;
        }
    }

    if (bad > 0) {
        printf("FAIL: %ld of %ld stopping points left damage the replay "
               "did not undo\n", bad, total + 1);
        printf("      the first is after %ld write(s): %s\n",
               first_bad, first_why);
        failures++;
    } else {
        printf("  %ld stopping points, and the replay left nothing to "
               "mend at any of them\n", total + 1);
    }

    free(image);
    if (failures == 0) {
        printf("all good\n");
    }
    return failures == 0 ? 0 : 1;
}

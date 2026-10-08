// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/mkext4.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * an ext4 image, with a directory tree in it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs/mkfs.h"
#include "fs/ext4.h"

#define SECTOR 512

static FILE *image;

static bool image_write(void *ctx, uint64_t lba, uint32_t count,
                        const void *buf)
{
    (void)ctx;
    if (fseek(image, (long)(lba * SECTOR), SEEK_SET) != 0) {
        return false;
    }
    return fwrite(buf, SECTOR, count, image) == count;
}

/*
 * writes here go straight to the file, so "the disk holds what has been
 * written to it" is already true and the promise the journal needs
 * costs nothing to make
 */
static bool host_sync(void *ctx)
{
    (void)ctx;
    return true;
}

static bool image_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (fseek(image, (long)(lba * SECTOR), SEEK_SET) != 0) {
        return false;
    }
    /*
     * past the end of what has been written is not an error: a fresh
     * image is a sparse file until something puts bytes in it, and the
     * driver is entitled to read a block it has only just allocated
     */
    size_t got = fread(buf, SECTOR, count, image);
    if (got < count) {
        memset((char *)buf + got * SECTOR, 0, (count - got) * SECTOR);
    }
    return true;
}

static uint32_t fixed_clock(void)
{
    /*
     * a fixed timestamp rather than the wall clock, so building the
     * same tree twice gives the same image. a build that churns is a
     * build whose output cannot be compared with anything
     */
    return 1700000000u;
}

static struct ext4 fs;
static int problems;

/*
 * every name in a directory, sorted, so the image does not depend on
 * the order the host filesystem happens to hand things back in
 */
static int list(const char *dir, char names[][256], int max)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < max) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        snprintf(names[count++], 256, "%s", e->d_name);
    }
    closedir(d);

    for (int i = 1; i < count; i++) {
        char keep[256];
        memcpy(keep, names[i], sizeof keep);
        int j = i;
        while (j > 0 && strcmp(names[j - 1], keep) > 0) {
            memcpy(names[j], names[j - 1], sizeof keep);
            j--;
        }
        memcpy(names[j], keep, sizeof keep);
    }
    return count;
}

static void copy_tree(const char *host_dir, const char *fs_dir)
{
    static char names[512][256];
    int count = list(host_dir, names, 512);

    for (int i = 0; i < count; i++) {
        char from[2048], to[2048];
        snprintf(from, sizeof from, "%s/%s", host_dir, names[i]);
        snprintf(to, sizeof to, "%s%s%s", fs_dir,
                 strcmp(fs_dir, "/") == 0 ? "" : "/", names[i]);

        struct stat st;
        if (lstat(from, &st) != 0) {
            continue;
        }

        if (S_ISLNK(st.st_mode)) {
            char target[1024];
            ssize_t n = readlink(from, target, sizeof target - 1);
            if (n < 0) {
                continue;
            }
            target[n] = '\0';
            /*
             * both kinds matter and only one of them is interesting: a
             * target under sixty bytes lives inside the inode itself,
             * and a longer one needs a block. the fixtures have both
             * because the short case is the one an implementation
             * forgets
             */
            if (!ext4_symlink(&fs, to, target, 0, 0)) {
                fprintf(stderr, "mkext4: cannot make the symlink %s\n", to);
                problems++;
            }
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!ext4_mkdir(&fs, to, 0755, 0, 0)) {
                fprintf(stderr, "mkext4: cannot make the directory %s\n", to);
                problems++;
                continue;
            }
            copy_tree(from, to);
            continue;
        }

        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        struct ext4_file f;
        uint32_t mode = (st.st_mode & 0100) ? 0755 : 0644;
        if (!ext4_create(&fs, to, mode, 0, 0, &f)) {
            fprintf(stderr, "mkext4: cannot make %s\n", to);
            problems++;
            continue;
        }

        FILE *src = fopen(from, "rb");
        if (src == NULL) {
            problems++;
            continue;
        }
        /*
         * written in pieces rather than whole, because the files that
         * matter here are the ones past twelve blocks, where an
         * indirect block appears, and past twelve plus two hundred
         * and fifty six, where a double indirect one does
         */
        static uint8_t buf[65536];
        uint64_t at = 0;
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
            int64_t wrote = ext4_write(&fs, &f, at, buf, n);
            if (wrote != (int64_t)n) {
                fprintf(stderr, "mkext4: %s stopped writing at %llu\n",
                        to, (unsigned long long)at);
                problems++;
                break;
            }
            at += n;
        }
        fclose(src);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "usage: mkext4 <image> <root-dir> <megabytes> "
                        "[label]\n");
        return 2;
    }
    const char *path = argv[1];
    const char *root = argv[2];
    uint64_t megabytes = strtoull(argv[3], NULL, 10);
    /* what the filesystem calls itself. */
    const char *label = (argc > 4) ? argv[4] : "velvetos";
    uint64_t sectors = megabytes * 1024 * 1024 / SECTOR;

    image = fopen(path, "w+b");
    if (image == NULL) {
        fprintf(stderr, "mkext4: cannot write %s\n", path);
        return 1;
    }
    /* the whole file, zeroed, before anything is laid out in it. */
    static const uint8_t zero[SECTOR];
    for (uint64_t i = 0; i < sectors; i++) {
        if (fwrite(zero, SECTOR, 1, image) != 1) {
            fprintf(stderr, "mkext4: cannot make the image %llu sectors\n",
                    (unsigned long long)sectors);
            return 1;
        }
    }

    struct mkfs_result r;
    const char *error = NULL;
    if (!mkfs_ext4(image_write, NULL, sectors, label, 1700000000, &r,
                   &error)) {
        fprintf(stderr, "mkext4: %s\n", error != NULL ? error : "cannot format");
        return 1;
    }

    memset(&fs, 0, sizeof fs);
    if (!ext4_mount(&fs, image_read, image_write, NULL)) {
        fprintf(stderr, "mkext4: cannot mount what was just formatted\n");
        return 1;
    }
    fs.clock = fixed_clock;

    /*
     * and the journal, which mounting deliberately leaves off: a disk
     * with a log mounts read-only until somebody promises the ordering
     * the log depends on. writes here go straight to the file, so "the
     * disk holds what has been written" is already true and the promise
     * costs nothing, but it is still made explicitly, because the
     * kernel's answer is a cache flush and is not free at all.
     *
     * after this, every file that goes in goes in through a
     * transaction. the log is therefore exercised thousands of times
     * building an image, rather than once in the life of a disk
     */
    ext4_set_sync(&fs, host_sync);
    if (!ext4_start_journal(&fs)) {
        fprintf(stderr, "mkext4: the journal would not start\n");
        return 1;
    }

    copy_tree(root, "/");

    /*
     * and closed again, which is what makes the image a *cleanly
     * unmounted* filesystem rather than one that merely stopped being
     * written to. the log is marked empty and the superblock stops
     * asking to be recovered, so whoever mounts this next, this
     * kernel or a linux, replays nothing because there is nothing to
     * replay. an image that shipped still claiming to need recovery
     * would be one every mount had to walk
     */
    if (!ext4_stop_journal(&fs)) {
        fprintf(stderr, "mkext4: the journal would not close cleanly\n");
        problems++;
    }

    fflush(image);
    fclose(image);

    if (problems > 0) {
        fprintf(stderr, "mkext4: %d things did not go in\n", problems);
        return 1;
    }
    printf("%s: %llu blocks, %llu inodes, %u group(s)\n", path,
           (unsigned long long)r.blocks, (unsigned long long)r.inodes,
           r.groups);
    return 0;
}

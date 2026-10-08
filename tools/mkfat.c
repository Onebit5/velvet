// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/mkfat.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a FAT32 image, with a directory tree in it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <sys/stat.h>

#include "fs/fat32.h"

#define SECTOR          512
#define RESERVED        32      /* boot, fsinfo, backup, and room to spare */
#define NUM_FATS        2
#define SEC_PER_CLUSTER 1

static FILE *image;

static bool image_write(void *ctx, uint64_t lba, uint32_t count,
                        const void *buf)
{
    (void)ctx;
    return fseek(image, (long)(lba * SECTOR), SEEK_SET) == 0
        && fwrite(buf, SECTOR, count, image) == count;
}

static bool image_read(void *ctx, uint64_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (fseek(image, (long)(lba * SECTOR), SEEK_SET) != 0) {
        return false;
    }
    size_t got = fread(buf, SECTOR, count, image);
    if (got < count) {
        memset((char *)buf + got * SECTOR, 0, (count - got) * SECTOR);
    }
    return true;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (i * 8));
    }
}

/* what a bios finds if it tries to boot a disk that is only a filesystem. */
static const uint8_t not_bootable_code[] = {
    0x31, 0xc0,                 /* xor ax, ax */
    0x8e, 0xd8,                 /* mov ds, ax */
    0x8e, 0xc0,                 /* mov es, ax */
    0xbe, 0x00, 0x00,           /* mov si, <patched> */
    0xac,                       /* lodsb            <- loop */
    0x84, 0xc0,                 /* test al, al */
    0x74, 0x06,                 /* jz halt */
    0xb4, 0x0e,                 /* mov ah, 0x0e */
    0xcd, 0x10,                 /* int 0x10 */
    0xeb, 0xf5,                 /* jmp loop */
    0xf4,                       /* hlt              <- halt */
    0xeb, 0xfd,                 /* jmp halt */
};
static const char not_bootable_msg[] =
    "this disk holds a filesystem and no bootloader\r\n";

static uint32_t fat_sectors_for(uint32_t total, uint32_t *clusters_out)
{
    for (uint32_t fat_sectors = 1; ; fat_sectors++) {
        long data = (long)total - RESERVED - NUM_FATS * (long)fat_sectors;
        long clusters = data / SEC_PER_CLUSTER;
        if (clusters <= 0) {
            fprintf(stderr, "mkfat: image too small to hold a filesystem\n");
            exit(1);
        }
        uint32_t entries = fat_sectors * SECTOR / 4;
        if (entries >= (uint32_t)clusters + 2) {
            *clusters_out = (uint32_t)clusters;
            return fat_sectors;
        }
    }
}

static void format(uint32_t total, const char *label)
{
    uint32_t clusters;
    uint32_t fat_sectors = fat_sectors_for(total, &clusters);

    uint8_t boot[SECTOR];
    memset(boot, 0, sizeof boot);
    boot[0] = 0xeb; boot[1] = 0x58; boot[2] = 0x90;
    memcpy(boot + 3, "VELVETOS", 8);
    put16(boot + 11, SECTOR);
    boot[13] = SEC_PER_CLUSTER;
    put16(boot + 14, RESERVED);
    boot[16] = NUM_FATS;
    put16(boot + 17, 0);                /* root entries: fat32 has none */
    put16(boot + 19, 0);                /* total sectors 16 */
    boot[21] = 0xf8;                    /* media descriptor */
    put16(boot + 22, 0);                /* fat size 16 */
    put16(boot + 24, 63);
    put16(boot + 26, 255);
    put32(boot + 28, 0);                /* hidden */
    put32(boot + 32, total);
    put32(boot + 36, fat_sectors);
    put16(boot + 40, 0);
    put16(boot + 42, 0);
    put32(boot + 44, 2);                /* the root directory's cluster */
    put16(boot + 48, 1);                /* fsinfo */
    put16(boot + 50, 6);                /* backup boot sector */
    boot[64] = 0x80;
    boot[66] = 0x29;
    put32(boot + 67, 0x54494e59);       /* volume id, 'TINY' */
    memset(boot + 71, ' ', 11);
    for (int i = 0; i < 11 && label[i] != '\0'; i++) {
        boot[71 + i] = (uint8_t)label[i];
    }
    memcpy(boot + 82, "FAT32   ", 8);

    memcpy(boot + 0x5a, not_bootable_code, sizeof not_bootable_code);
    uint16_t msg_at = (uint16_t)(0x7c00 + 0x5a + sizeof not_bootable_code);
    put16(boot + 0x5a + 7, msg_at);
    memcpy(boot + 0x5a + sizeof not_bootable_code, not_bootable_msg,
           sizeof not_bootable_msg);
    boot[510] = 0x55; boot[511] = 0xaa;

    uint8_t fsinfo[SECTOR];
    memset(fsinfo, 0, sizeof fsinfo);
    memcpy(fsinfo, "RRaA", 4);
    memcpy(fsinfo + 484, "rrAa", 4);
    put32(fsinfo + 488, clusters - 1);  /* free, with the root taken */
    put32(fsinfo + 492, 3);             /* where to look next */
    fsinfo[510] = 0x55; fsinfo[511] = 0xaa;

    image_write(NULL, 0, 1, boot);
    image_write(NULL, 1, 1, fsinfo);
    image_write(NULL, 6, 1, boot);      /* the backup, which fsck wants */

    /*
     * the tables. the first two entries are reserved, the media
     * descriptor and an end mark, and cluster 2 is the root
     * directory, which is one cluster long and ends there
     */
    uint8_t sector[SECTOR];
    memset(sector, 0, sizeof sector);
    put32(sector + 0, 0x0ffffff8);
    put32(sector + 4, 0x0fffffff);
    put32(sector + 8, 0x0fffffff);
    for (int copy = 0; copy < NUM_FATS; copy++) {
        uint64_t at = RESERVED + (uint64_t)copy * fat_sectors;
        image_write(NULL, at, 1, sector);
        uint8_t zero[SECTOR];
        memset(zero, 0, sizeof zero);
        for (uint32_t i = 1; i < fat_sectors; i++) {
            image_write(NULL, at + i, 1, zero);
        }
    }

    /*
     * and the root directory's cluster, zeroed, so the first entry read
     * out of it is an end-of-directory marker rather than whatever the
     * image happened to contain
     */
    uint8_t zero[SECTOR];
    memset(zero, 0, sizeof zero);
    uint64_t data = RESERVED + (uint64_t)NUM_FATS * fat_sectors;
    image_write(NULL, data, SEC_PER_CLUSTER, zero);

    printf("%u sectors, %u clusters, %u sectors of table each\n",
           total, clusters, fat_sectors);
}

static struct fat32 fs;
static int problems;

static int list(const char *dir, char names[][256], int max)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    int count = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && count < max) {
        if (e->d_name[0] == '.') {
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
        if (stat(from, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (!fat32_mkdir(&fs, to)) {
                fprintf(stderr, "mkfat: cannot make the directory %s\n", to);
                problems++;
                continue;
            }
            copy_tree(from, to);
            continue;
        }
        if (!S_ISREG(st.st_mode)) {
            continue;
        }

        struct fat32_file f;
        if (!fat32_create(&fs, to, &f)) {
            fprintf(stderr, "mkfat: cannot make %s\n", to);
            problems++;
            continue;
        }
        FILE *src = fopen(from, "rb");
        if (src == NULL) {
            problems++;
            continue;
        }
        static uint8_t buf[65536];
        uint64_t at = 0;
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, src)) > 0) {
            if (fat32_write(&fs, &f, at, buf, n) != (int64_t)n) {
                fprintf(stderr, "mkfat: %s stopped writing at %llu\n", to,
                        (unsigned long long)at);
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
        fprintf(stderr, "usage: mkfat <image> <root-dir> <megabytes>\n");
        return 2;
    }
    uint64_t megabytes = strtoull(argv[3], NULL, 10);
    uint32_t total = (uint32_t)(megabytes * 1024 * 1024 / SECTOR);

    image = fopen(argv[1], "w+b");
    if (image == NULL) {
        fprintf(stderr, "mkfat: cannot write %s\n", argv[1]);
        return 1;
    }
    static const uint8_t zero[SECTOR];
    for (uint32_t i = 0; i < total; i++) {
        if (fwrite(zero, SECTOR, 1, image) != 1) {
            fprintf(stderr, "mkfat: cannot make the image that large\n");
            return 1;
        }
    }

    printf("%s: ", argv[1]);
    format(total, "VELVETOS");

    memset(&fs, 0, sizeof fs);
    if (!fat32_mount(&fs, image_read, image_write, NULL)) {
        fprintf(stderr, "mkfat: cannot mount what was just formatted\n");
        return 1;
    }
    copy_tree(argv[2], "/");

    /* and the free count, now that it is known. */
    uint32_t used, all;
    if (fat32_usage(&fs, &used, &all)) {
        uint8_t fsinfo[SECTOR];
        if (image_read(NULL, 1, 1, fsinfo)) {
            put32(fsinfo + 488, all - used);
            image_write(NULL, 1, 1, fsinfo);
        }
    }

    fflush(image);
    fclose(image);

    if (problems > 0) {
        fprintf(stderr, "mkfat: %d things did not go in\n", problems);
        return 1;
    }
    return 0;
}

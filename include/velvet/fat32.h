// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/fat32.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * fat32: a filesystem on a disk that survives a reboot.
 */

/* the design notes for fat32.h are in docs/subsystems/fs.rst */

#ifndef VELVET_FAT32_H
#define VELVET_FAT32_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define FAT32_SECTOR   512
#define FAT32_NAME_MAX 128

/*
 * what time it is, for the fields fat has always had and this kernel has
 * always written zeroes into. a function rather than a call to the clock
 * because the filesystem touches no hardware, and because a test with
 * a fixed clock can then say exactly what should have been written
 */
struct fat32_time {
    uint16_t year;          /* 1980..2107, which is all fat can hold */
    uint8_t  month, day;
    uint8_t  hour, minute, second;
};

typedef void (*fat32_clock)(struct fat32_time *out);

typedef bool (*fat32_io)(void *ctx, uint64_t lba, uint32_t count, void *buf);
typedef bool (*fat32_out)(void *ctx, uint64_t lba, uint32_t count,
                          const void *buf);

struct fat32 {
    fat32_io  read;
    fat32_out write;            /* NULL for a disk the kernel may only read */
    fat32_clock clock;          /* NULL means every stamp comes out zero */
    void     *ctx;

    uint32_t sectors_per_cluster;
    uint32_t reserved_sectors;
    uint32_t num_fats;
    uint32_t fat_sectors;
    uint32_t root_cluster;
    uint32_t total_sectors;

    /* worked out from those, because everything else needs them */
    uint64_t first_data_sector;
    uint32_t cluster_count;

    char     label[12];
    bool     mounted;

    uint8_t  scratch[FAT32_SECTOR];
};

struct fat32_file {
    char     name[FAT32_NAME_MAX];
    uint32_t first_cluster;
    uint32_t size;
    uint8_t  attr;
    bool     is_dir;

    uint64_t entry_sector;
    uint32_t entry_offset;

    /*
     * where the long-name entries in front of it begin, so that
     * removing the file can remove them too rather than leaving a run
     * of orphans pointing at a name that is gone
     */
    uint64_t lfn_sector;
    uint32_t lfn_offset;

    struct fat32_time written;
};

#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0f

bool fat32_mount(struct fat32 *fs, fat32_io read, fat32_out write, void *ctx);

void fat32_set_clock(struct fat32 *fs, fat32_clock clock);

bool fat32_lookup(struct fat32 *fs, const char *path, struct fat32_file *out);

bool fat32_readdir(struct fat32 *fs, uint32_t dir_cluster, size_t index,
                   struct fat32_file *out);

int64_t fat32_read(struct fat32 *fs, const struct fat32_file *f,
                   uint64_t offset, void *buf, uint64_t len);

int64_t fat32_write(struct fat32 *fs, struct fat32_file *f,
                    uint64_t offset, const void *buf, uint64_t len);

bool fat32_create(struct fat32 *fs, const char *path, struct fat32_file *out);

bool fat32_mkdir(struct fat32 *fs, const char *path);

bool fat32_rmdir(struct fat32 *fs, const char *path);

bool fat32_unlink(struct fat32 *fs, const char *path);

/*
 * rename, or move. within one directory it rewrites eleven bytes; across
 * directories it writes a new entry pointing at the same clusters and
 * strikes out the old one, no data is copied either way, because the
 * file never moves. only the name does
 */
bool fat32_rename(struct fat32 *fs, const char *from, const char *to);

bool fat32_usage(struct fat32 *fs, uint32_t *used, uint32_t *total);

uint32_t fat32_cluster_bytes(const struct fat32 *fs);

#endif

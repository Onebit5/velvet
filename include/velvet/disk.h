// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/disk.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the disk, mounted.
 */

/* the design notes for this file are in docs/subsystems/fs.rst */

#ifndef VELVET_DISK_H

#define VELVET_DISK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <velvet/fat32.h>
#include <velvet/ext4.h>
#include <velvet/bcache.h>
#include <velvet/fsck.h>
#include <velvet/part.h>

/*
 * the disk, mounted.
 *
 * one place that owns the sata controller and the filesystem on it, so
 * that nothing above has to know which is which. paths are absolute and
 * rooted at the disk itself; deciding that a name belongs here at all
 * is the vfs's job, not this file's.
 *
 * everything degrades quietly. a machine with no disk, or a disk with
 * no filesystem the kernel recognises, answers false to all of this and boots
 * exactly as it did before there was any of it.
 */

#define DISK_NAME_MAX   128

/* a disk is not a filesystem, and the machine got a partition table. */
#define DISK_MOUNTS     2
#define DISK_ROOT       0
#define DISK_WORK       1
#define DISK_WORK_AT    "/work"

const char *disk_mount_point(size_t which);

struct disk_entry {
    char     name[DISK_NAME_MAX];
    uint64_t size;
    uint32_t cluster;
    bool     is_dir;

    uint64_t entry_sector;
    uint32_t entry_offset;

    struct fat32_time written;

    /*
     * on a fat disk these are the mount's answer rather than the
     * file's, because fat has nowhere to keep them and inventing a
     * per-file answer would be a lie with a number in it. on an ext4
     * one they come out of the inode, which is the entire reason that
     * happened
     */
    uint32_t mode;              /* permissions, without the type bits */
    uint32_t uid, gid;
    bool     is_symlink;
    uint32_t ino;               /* the identity. 0 where there is none */
};

enum disk_kind {
    DISK_NONE = 0,
    DISK_FAT32,
    DISK_EXT4,
};

enum disk_kind disk_which(size_t which);
const char *disk_kind_name(size_t which);

/* a disk is not a filesystem, so "which disk" was never the right question. */

struct disk_part {
    struct partition p;
    enum part_scheme scheme;    /* how it was found, or PART_NONE for a
                                 * whole drive with no table */
    bool     mountable;         /* something recognised a filesystem on it */
    const char *fs;             /* what that was, or "" */
};

size_t disk_part_count(void);
bool   disk_part_at(size_t index, struct disk_part *out);

int disk_mounted_part(size_t which);

bool disk_mount_part(size_t which, size_t index);

bool disk_mount(void);

bool disk_ready(size_t which);

bool disk_lookup(size_t which, const char *path, struct disk_entry *out);

bool disk_readdir(size_t which, const char *path, size_t index,
                  struct disk_entry *out);

int64_t disk_read(size_t which, uint32_t cluster, uint64_t size,
                  uint64_t offset, void *buf, uint64_t len);

/* make a file if it is not there, and hand back where it lives. */
bool disk_create(size_t which, const char *path, struct disk_entry *out);

int64_t disk_write_at(size_t which, struct disk_entry *e, uint64_t offset,
                      const void *buf, uint64_t len);

bool disk_mkdir(size_t which, const char *path);
bool disk_rmdir(size_t which, const char *path);

bool disk_unlink(size_t which, const char *path);

bool disk_rename(size_t which, const char *from, const char *to);

bool disk_chmod(size_t which, const char *path, uint32_t mode);
bool disk_chown(size_t which, const char *path, uint32_t uid, uint32_t gid);
bool disk_symlink(size_t which, const char *path, const char *target);

bool disk_readlink(size_t which, const char *path, char *out, size_t size);
bool disk_lookup_nofollow(size_t which, const char *path,
                          struct disk_entry *out);

bool disk_sync(void);

bool disk_unmount(void);

size_t   disk_drive_count(void);
uint64_t disk_drive_sectors(unsigned drive);
const char *disk_drive_model(unsigned drive);

bool disk_raw_read(unsigned drive, uint64_t lba, uint32_t count, void *buf);
bool disk_raw_write(unsigned drive, uint64_t lba, uint32_t count,
                    const void *buf);

int disk_mounted_drive(void);

bool disk_system_read(void *ctx, uint64_t lba, uint32_t count, void *buf);

int disk_system_drive(void);

/* is there anything to lose? cheap enough to ask on a timer */
bool disk_fsck(size_t which, bool mend, struct fsck_report *out,
               const char **error);

/*
 * the filesystem describes the disk it was made for, and its partition
 * is usually longer than that. growing it is the driver's work; how
 * much room there is belongs here, because how long a partition is is
 * not something a filesystem handed a view of one may know
 */
bool disk_resize(size_t which, uint64_t blocks, struct ext4_resize *out,
                 const char **error);

uint64_t disk_room(size_t which);
uint64_t disk_ceiling(size_t which);

bool disk_dirty(void);

void disk_cache_stats(struct bcache_stats *out);

const char *disk_label(size_t which);
const char *disk_model(void);
uint64_t    disk_bytes(void);
bool        disk_usage(size_t which, uint64_t *used_bytes,
                       uint64_t *total_bytes);
uint32_t    disk_cluster_bytes(size_t which);
bool        disk_journalled(size_t which);

#endif

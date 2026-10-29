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

/*
 * a disk is not a filesystem, and the machine got a partition table:
 * a filesystem is mounted in a slot of its own, numbered, by mount
 */
#define DISK_MOUNTS     8

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

/*
 * the drives found, and what each of their tables says it holds, listed
 * as block devices: whether there is a drive at all. nothing is mounted
 */
bool disk_scan(void);

bool disk_ready(size_t which);

bool disk_lookup(size_t which, const char *path, struct disk_entry *out);

bool disk_readdir(size_t which, const char *path, size_t index,
                  struct disk_entry *out);

int64_t disk_read(size_t which, uint32_t cluster, uint64_t size,
                  uint64_t offset, void *buf, uint64_t len);

/* make a file if it is not there, and hand back where it lives. */
bool disk_create(size_t which, const char *path, uint32_t u_mode,
                 uint32_t u_uid, uint32_t u_gid, struct disk_entry *out);

int64_t disk_write_at(size_t which, struct disk_entry *e, uint64_t offset,
                      const void *buf, uint64_t len);

/* the file made size long, its record updated to match */
bool disk_truncate(size_t sz_which, struct disk_entry *p_entry,
                   uint64_t u_size);

bool disk_mkdir(size_t which, const char *path, uint32_t u_mode,
                uint32_t u_uid, uint32_t u_gid);
bool disk_rmdir(size_t which, const char *path);

bool disk_unlink(size_t which, const char *path);

bool disk_rename(size_t which, const char *from, const char *to);

bool disk_chmod(size_t which, const char *path, uint32_t mode);
bool disk_chown(size_t which, const char *path, uint32_t uid, uint32_t gid);
bool disk_symlink(size_t which, const char *path, const char *target,
                  uint32_t u_uid, uint32_t u_gid);

bool disk_readlink(size_t which, const char *path, char *out, size_t size);
bool disk_lookup_nofollow(size_t which, const char *path,
                          struct disk_entry *out);

bool disk_sync(void);

bool disk_unmount(void);

/*
 * the drives, and the partitions their tables name, as block devices:
 * every drive first, in the order its port is, then every partition in
 * the order the scan found it. each is known by its number, major 8 and
 * minor drive * 16 + partition, which is how a program names one, and a
 * partition past the fifteenth has no number and is not one
 */
#define DISK_BLOCK_MAJOR    8
#define DISK_BLOCK_NAME_MAX 8

struct disk_block {
    char     arr_name[DISK_BLOCK_NAME_MAX];     /* sda, or sda1 */
    uint32_t u_rdev;                            /* major << 8 | minor */
    unsigned u_drive;
    uint64_t u_first, u_sectors;
};

/* how many, which is which, or NULL past the end or for no such number */
size_t disk_block_count(void);
const struct disk_block *disk_block_at(size_t sz_index);
const struct disk_block *disk_block_find(uint32_t u_rdev);

/*
 * bytes of one, from u_offset on, through the block cache, so what a
 * program reads is what a filesystem on it last wrote and what it writes
 * is what that filesystem reads next: how many, 0 for a read at the end,
 * -ENOSPC for a write that starts there, or -EIO
 */
int64_t disk_block_read(const struct disk_block *p_block, uint64_t u_offset,
                        void *p_buf, uint64_t u_len);
int64_t disk_block_write(const struct disk_block *p_block, uint64_t u_offset,
                         const void *p_buf, uint64_t u_len);

/*
 * the filesystem on a drive or a partition, mounted in a slot of its own:
 * which slot, -EBUSY when its sectors are mounted already, -EINVAL when it
 * holds no filesystem of the kind wanted, or -ENOMEM with every slot
 * taken. e_want is DISK_EXT4, DISK_FAT32, or DISK_NONE for whichever
 */
int disk_mount_block(const struct disk_block *p_block, enum disk_kind e_want);

/* a slot's filesystem written out, its log closed, and the slot free */
bool disk_unmount_slot(size_t sz_which);


int disk_system_drive(void);

/* the number of the drive or partition a slot's filesystem is on, or 0 */
uint32_t disk_dev(size_t sz_which);

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

/* is there anything to lose? cheap enough to ask on a timer */
bool disk_dirty(void);

void disk_cache_stats(struct bcache_stats *out);

const char *disk_label(size_t which);
bool        disk_usage(size_t which, uint64_t *used_bytes,
                       uint64_t *total_bytes);
uint32_t    disk_cluster_bytes(size_t which);
/* how many files it has room for, and how many more, 0 for none kept */
bool        disk_files(size_t which, uint64_t *p_files, uint64_t *p_free);
bool        disk_journalled(size_t which);

#endif

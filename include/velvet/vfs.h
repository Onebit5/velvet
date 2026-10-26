// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/vfs.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * one namespace, two filesystems underneath it.
 */

/* the design notes for vfs.h are in docs/subsystems/fs.rst */

#ifndef VELVET_VFS_H
#define VELVET_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <velvet/disk.h>

#define VFS_NAME_MAX 128
#define VFS_BOOT     "/boot"


enum vfs_kind {
    VFS_NOWHERE = 0,
    VFS_RAMDISK,
    VFS_DISK,
    VFS_PROC,       /* written by the kernel when read, see fs/proc.c */
    VFS_DEV,        /* a device, opened through fs/dev.c */
};

struct vfs_file {
    enum vfs_kind kind;

    size_t mount;

    char     name[VFS_NAME_MAX];
    uint64_t size;
    bool     is_dir;

    uint32_t mode;
    uint32_t uid, gid;
    bool     is_symlink;
    uint32_t u_ino;         /* the inode, where a filesystem has them */

    const void *data;
    uint32_t    cluster;
    uint64_t    entry_sector;
    uint32_t    entry_offset;

    struct fat32_time written;

    /* which /proc file, for VFS_PROC */
    size_t sz_proc;
    size_t sz_dev;          /* which device, for VFS_DEV */
};

bool vfs_open(const char *path, struct vfs_file *out);

bool vfs_readdir(const char *path, size_t index, struct vfs_file *out);

/* make a file. only the disk can, and it says so when it cannot */
bool vfs_create(const char *path, uint32_t u_mode, uint32_t u_uid,
                uint32_t u_gid, struct vfs_file *out);

bool vfs_mkdir(const char *path, uint32_t u_mode, uint32_t u_uid,
               uint32_t u_gid);
bool vfs_rmdir(const char *path);

bool vfs_unlink(const char *path);

/*
 * rename, which is also how a file is moved: both are one name being
 * replaced by another, and neither copies a byte. within one mount
 * only, a name cannot move from the ramdisk to the disk, because
 * that would be a copy wearing a rename's clothes
 */
bool vfs_rename(const char *from, const char *to);

bool vfs_chmod(const char *path, uint32_t mode);
bool vfs_chown(const char *path, uint32_t uid, uint32_t gid);
bool vfs_symlink(const char *path, const char *target, uint32_t u_uid,
                 uint32_t u_gid);

bool vfs_readlink(const char *path, char *out, size_t size);
bool vfs_open_nofollow(const char *path, struct vfs_file *out);

int64_t vfs_read(const struct vfs_file *f, uint64_t offset, void *buf,
                 uint64_t len);
int64_t vfs_write(struct vfs_file *f, uint64_t offset, const void *buf,
                  uint64_t len);

/*
 * a file on a disk made size long: zeroes past the old end, and the
 * blocks past the new one given back. false on the ramdisk, in /proc, and
 * for a directory
 */
bool vfs_truncate(struct vfs_file *p_node, uint64_t u_size);

bool vfs_may_read(const struct vfs_file *f, int uid);

bool vfs_writable(const struct vfs_file *f);

/*
 * the whole of a file in one piece, for the things that need it all at
 * once, loading a program, reading passwd. a file already in memory
 * is handed over where it lies and `owned` comes back false; one on the
 * disk is read into the heap and the caller has to free it
 */
bool vfs_slurp(const char *path, const void **data, uint64_t *size,
               bool *owned);
void vfs_release(const void *data, bool owned);

struct file;

/*
 * a file on a filesystem, opened for u_mode (FILE_READ, FILE_WRITE),
 * positioned at its start, with one reference. the caller has already
 * decided the opener may
 */
struct file *vfs_open_file(const struct vfs_file *p_node, uint32_t u_mode);

struct vfs_mount {
    const char *at;
    const char *what;
    const char *where;
    bool        writable;
    bool        present;
};

size_t vfs_mount_count(void);
bool   vfs_mount_at(size_t index, struct vfs_mount *out);

#endif

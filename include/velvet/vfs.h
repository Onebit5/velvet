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
#include <velvet/path.h>

#define VFS_NAME_MAX 128


enum vfs_kind {
    VFS_NOWHERE = 0,
    VFS_DISK,
    VFS_PROC,       /* written by the kernel when read, see fs/proc.c */
    VFS_DEV,        /* a device, opened through fs/dev.c */
    VFS_RAMFS,      /* a filesystem kept in memory, see fs/ramfs.c */
};

/* what mount is told a filesystem is, kept to show in /proc/mounts */
#define VFS_SOURCE_MAX 64

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
    uint32_t u_type;        /* S_IFCHR and the rest, where a node has one */
    uint32_t u_rdev;        /* a device node's number, major << 8 | minor */

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

/* a name something is mounted at, which stays where it is */
bool vfs_is_mount_point(const char *str_path);

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

/*
 * a device node, by the type in u_mode, numbered u_rdev: false where the
 * filesystem has nowhere to keep one
 */
bool vfs_mknod(const char *str_path, uint32_t u_mode, uint32_t u_uid,
               uint32_t u_gid, uint32_t u_rdev);

/*
 * whether opening it opens a device rather than a file: one in /dev, or a
 * node naming one by its number on any filesystem
 */
bool vfs_is_device(const struct vfs_file *p_node);

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
 * a node's size brought up to date, for one that may have been written
 * through another open file since it was looked up
 */
void vfs_refresh(struct vfs_file *p_node);

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

/* one mount, as the table has it, copied out */
struct vfs_mount {
    char        arr_at[PATH_MAX];
    char        arr_where[VFS_SOURCE_MAX];
    const char *what;
    bool        writable;
    bool        present;
    enum vfs_kind e_kind;
    size_t      sz_slot;
};

size_t vfs_mount_count(void);
bool   vfs_mount_at(size_t index, struct vfs_mount *out);

/*
 * where the filesystem of e_kind numbered sz_slot is mounted, into
 * str_out: true, or false and "?"
 */
bool vfs_where(enum vfs_kind e_kind, size_t sz_slot, char *str_out,
               size_t sz_size);

/*
 * a filesystem hung at str_at, an absolute path, recorded as str_source:
 * 0, -EBUSY when something hangs there already, or -ENOMEM with the
 * table full. whether there is a directory there is the caller's to ask
 */
int64_t vfs_mount_add(const char *str_at, enum vfs_kind e_kind,
                      size_t sz_slot, const char *str_source);

/*
 * the mount at str_at taken down, with what was there in *p_kind and
 * *p_slot for the caller to let go of: 0, -EINVAL when nothing is
 * mounted there, or -EBUSY for the root, or one something stands on, is
 * open on, or works in
 */
int64_t vfs_mount_remove(const char *str_at, enum vfs_kind *p_kind,
                         size_t *p_slot);

/*
 * the root at boot: a filesystem in memory with the archive the loader
 * handed over unpacked into it, and /dev on it. how many of the
 * archive's entries it holds in *p_files: false with no memory for it
 */
bool vfs_root_init(size_t *p_files);

/*
 * a mount, and every mount on it, moved from str_from to str_to, and
 * every path anybody holds under it moved with it: 0, -EINVAL when
 * nothing is mounted at str_from, it is the root, or str_to is under it,
 * or -EBUSY when something is mounted at str_to or a call is in the
 * middle of a name under str_from
 */
int64_t vfs_mount_move(const char *str_from, const char *str_to);

/*
 * the mount at str_new made the root, and the old root put at str_old,
 * which is under str_new, with every mount not under str_new going with
 * it: 0, -EINVAL when nothing is mounted at str_new, it is the root
 * already, or str_old is not under it, or -EBUSY when something is
 * mounted at str_old or a call is in the middle of a name
 */
int64_t vfs_pivot(const char *str_new, const char *str_old);

#endif

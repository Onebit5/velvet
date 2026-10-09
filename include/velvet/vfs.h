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

/*
 * the source tree, which is under /boot because that is where it is:
 * the same medium philemon read the kernel and the ramdisk off, at
 * sectors nothing else looks at. it is not *in* the ramdisk, the
 * ramdisk is in memory and this is emphatically not, so it is a
 * mount of its own standing in the ramdisk's directory, the way /work
 * stands in the root's
 */
#define VFS_SRC      "/boot/src"

enum vfs_kind {
    VFS_NOWHERE = 0,
    VFS_RAMDISK,
    VFS_DISK,
    VFS_SOURCE,
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

    const void *data;
    uint32_t    cluster;
    uint64_t    entry_sector;
    uint32_t    entry_offset;

    /*
     * or on the boot medium, at a byte offset with no filesystem
     * anywhere near it, which is the third answer and
     * the reason this is a field rather than a reuse of one above. a
     * tar puts a file wherever the one before it ended
     */
    uint64_t    medium_at;

    struct fat32_time written;
};

bool vfs_open(const char *path, struct vfs_file *out);

bool vfs_readdir(const char *path, size_t index, struct vfs_file *out);

/* make a file. only the disk can, and it says so when it cannot */
bool vfs_create(const char *path, struct vfs_file *out);

bool vfs_mkdir(const char *path);
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
bool vfs_symlink(const char *path, const char *target);

bool vfs_readlink(const char *path, char *out, size_t size);
bool vfs_open_nofollow(const char *path, struct vfs_file *out);

int64_t vfs_read(const struct vfs_file *f, uint64_t offset, void *buf,
                 uint64_t len);
int64_t vfs_write(struct vfs_file *f, uint64_t offset, const void *buf,
                  uint64_t len);

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

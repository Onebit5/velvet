// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/ramfs.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a filesystem kept in memory.
 */

/* the design notes for ramfs.h are in docs/subsystems/fs.rst */

#ifndef VELVET_RAMFS_H
#define VELVET_RAMFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define RAMFS_MAX       8       /* filesystems, all of them at once */
#define RAMFS_NAME_MAX  128     /* one name in a directory, NUL included */
#define RAMFS_PATH_MAX  256

/*
 * what a name leads to. u_mode carries the type with the permissions,
 * S_IFREG, S_IFDIR, S_IFLNK and the devices, the way stat gives them,
 * and u_ino is what the file is known by from here on: every call that
 * reads or writes one takes its number rather than its name
 */
struct ramfs_entry {
    char     arr_name[RAMFS_NAME_MAX];
    uint32_t u_ino;
    uint32_t u_mode;
    uint32_t u_uid, u_gid;
    uint32_t u_rdev;            /* a device's number, major << 8 | minor */
    uint64_t u_size;            /* a link's is its target's length */
    uint64_t u_mtime;           /* seconds since 1970 */
};

/*
 * a new filesystem holding nothing but its root, root's, with u_mode for
 * its permissions: which one, or -1 when there are RAMFS_MAX already or
 * no memory
 */
int ramfs_new(uint32_t u_mode);

/* whether a file on it is open, which is what keeps it from going */
bool ramfs_busy(size_t sz_fs);

/* all of it given back, unless something on it is open */
bool ramfs_free(size_t sz_fs);

/*
 * what is at a path, from the filesystem's root, following links on the
 * way and at the end when b_follow
 */
bool ramfs_lookup(size_t sz_fs, const char *str_path, bool b_follow,
                  struct ramfs_entry *p_out);

/* entry sz_index of a directory, in the order they were made */
bool ramfs_readdir(size_t sz_fs, const char *str_path, size_t sz_index,
                   struct ramfs_entry *p_out);

/*
 * a new name: a file, a directory, a link to str_target, or a device or
 * a fifo numbered u_rdev, by the type in u_mode. false when the name is
 * there already, where it goes is not a directory, or there is no memory
 */
bool ramfs_make(size_t sz_fs, const char *str_path, uint32_t u_mode,
                uint32_t u_uid, uint32_t u_gid, uint32_t u_rdev,
                const char *str_target, struct ramfs_entry *p_out);

/*
 * a name that is not a directory, gone. a file still open stays, with no
 * name, until the last close of it
 */
bool ramfs_unlink(size_t sz_fs, const char *str_path);

/* a directory with nothing in it, gone */
bool ramfs_rmdir(size_t sz_fs, const char *str_path);

/*
 * a name moved, over whatever was at str_to if that is the same kind of
 * thing and, for a directory, empty. never into itself
 */
bool ramfs_rename(size_t sz_fs, const char *str_from, const char *str_to);

bool ramfs_chmod(size_t sz_fs, const char *str_path, uint32_t u_mode);
bool ramfs_chown(size_t sz_fs, const char *str_path, uint32_t u_uid,
                 uint32_t u_gid);
bool ramfs_readlink(size_t sz_fs, const char *str_path, char *str_out,
                    size_t sz_size);

/*
 * a file's bytes, by its number: how many, 0 at the end, -ENOSPC when
 * memory runs out before any were written, or -EINVAL for a number that
 * is no file's
 */
int64_t ramfs_read(uint32_t u_ino, uint64_t u_offset, void *p_buf,
                   uint64_t u_len);
int64_t ramfs_write(uint32_t u_ino, uint64_t u_offset, const void *p_buf,
                    uint64_t u_len);

/* a file made u_size long: zeroes past the old end */
bool ramfs_truncate(uint32_t u_ino, uint64_t u_size);

/* how long a file is now, by its number, whoever has made it so */
bool ramfs_size(uint32_t u_ino, uint64_t *p_size);

/* one more open file on it, and one fewer */
void ramfs_hold(uint32_t u_ino);
void ramfs_let_go(uint32_t u_ino);

/* how many bytes its files hold, and how many names it has */
void ramfs_usage(size_t sz_fs, uint64_t *p_bytes, uint64_t *p_nodes);

/* how many more names all of them together have room for */
uint64_t ramfs_room(void);

#endif

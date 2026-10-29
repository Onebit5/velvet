// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/namei.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a name a program hands over, turned into a file, and who may use it.
 */

#ifndef VELVET_NAMEI_H
#define VELVET_NAMEI_H

#include <stdint.h>
#include <stdbool.h>

#include <velvet/path.h>
#include <velvet/vfs.h>

/*
 * a path out of ring 3 made absolute: against i_dirfd, a directory
 * descriptor, or the working directory for AT_FDCWD, and against
 * nothing when it starts with a slash. 0, or -EFAULT, -ENAMETOOLONG,
 * -ENOENT for an empty one, -EBADF or -ENOTDIR for a dirfd that is not
 * an open directory
 */
int64_t path_at(int i_dirfd, uint64_t u_path, char *str_out);

/*
 * a path in the namespace as a process whose "/" is str_root sees it,
 * into PATH_MAX of str_out: what is under the root without it, and "/"
 * for anything that is not, which it has no name for
 */
void path_seen_from(const char *str_root, const char *str_path,
                    char *str_out);

/*
 * what is at an absolute path, following a link at the end or not: 0,
 * -ENOENT, -ENOTDIR when a directory on the way is not one, or -EACCES
 * when one on the way may not be searched by the caller
 */
int64_t path_lookup(const char *str_path, bool b_follow,
                    struct vfs_file *p_out);

/*
 * the directory a new name would go in, ready for one: 0, or what is in
 * the way, from -ENOENT to -EACCES for a directory the caller may not
 * write, -EROFS for one nothing may write, and -EEXIST when the name is
 * there already and b_must_be_new
 */
int64_t may_make(const char *str_path, bool b_must_be_new);

/*
 * may the caller do i_mask, R_OK, W_OK and X_OK together, to the file?
 * root may do anything but run a file nothing may run
 */
bool may_access(const struct vfs_file *p_node, int i_mask);

/* who the caller is */
int current_uid(void);
int current_gid(void);

/*
 * a number for a file that has no inode of its own to give, the same
 * for the same path every time it is asked
 */
uint64_t path_ino(const char *str_path);

#endif

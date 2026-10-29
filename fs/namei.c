// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/namei.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * names: resolved, looked up, checked against who asks, made and unmade.
 */

#include <velvet/namei.h>
#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/file.h>
#include <velvet/process.h>
#include <velvet/string.h>
#include <asm/uaccess.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/stat.h>

int current_uid(void)
{
    const struct process *p = process_find(current_pid());
    return (p != NULL) ? p->uid : 0;
}

int current_gid(void)
{
    const struct process *p = process_find(current_pid());
    return (p != NULL) ? p->i_gid : 0;
}

static bool in_group(const struct process *p, uint32_t u_gid)
{
    if ((uint32_t)p->i_gid == u_gid) {
        return true;
    }
    for (int i_at = 0; i_at < p->i_groups; i_at++) {
        if ((uint32_t)p->arr_groups[i_at] == u_gid) {
            return true;
        }
    }
    return false;
}

bool may_access(const struct vfs_file *p_node, int i_mask)
{
    const struct process *p = process_find(current_pid());
    if (p == NULL || p->uid == 0) {
        /* root runs only what somebody may run, and reads and writes all */
        return !(i_mask & X_OK) || p_node->is_dir
               || (p_node->mode & 0111) != 0;
    }
    uint32_t u_bits;
    if (p_node->uid == (uint32_t)p->uid) {
        u_bits = (p_node->mode >> 6) & 7;
    } else if (in_group(p, p_node->gid)) {
        u_bits = (p_node->mode >> 3) & 7;
    } else {
        u_bits = p_node->mode & 7;
    }
    return (u_bits & (uint32_t)i_mask) == (uint32_t)i_mask;
}

uint64_t path_ino(const char *str_path)
{
    uint64_t u_hash = 1469598103934665603ull;
    for (const char *p_at = str_path; *p_at != '\0'; p_at++) {
        u_hash = (u_hash ^ (uint8_t)*p_at) * 1099511628211ull;
    }
    return (u_hash & 0x7fffffffull) | 0x80000000ull;
}

void path_seen_from(const char *str_root, const char *str_path,
                    char *str_out)
{
    size_t sz_root = strlen(str_root);
    const char *str_below = str_path + sz_root;

    if (sz_root == 1) {
        str_below = str_path;
    } else if (strncmp(str_path, str_root, sz_root) != 0
               || (*str_below != '\0' && *str_below != '/')) {
        str_below = "/";
    } else if (*str_below == '\0') {
        str_below = "/";
    }
    strncpy(str_out, str_below, PATH_MAX - 1);
    str_out[PATH_MAX - 1] = '\0';
}

/*
 * a name as the caller sees it, resolved inside its root so .. stops
 * there, and then put under that root, which is what it names in the
 * namespace
 */
static int64_t under_root(const char *str_root, const char *str_base,
                          const char *str_raw, char *str_out)
{
    char arr_view[PATH_MAX];
    char arr_inside[PATH_MAX];

    path_seen_from(str_root, str_base, arr_view);
    if (!path_resolve(arr_view, str_raw, arr_inside, PATH_MAX)) {
        return -ENAMETOOLONG;
    }
    /* a root's own "/" is the root itself, with nothing after it */
    size_t sz_root = (strcmp(str_root, "/") == 0) ? 0 : strlen(str_root);
    const char *str_tail = (sz_root > 0 && strcmp(arr_inside, "/") == 0)
                           ? "" : arr_inside;
    size_t sz_tail = strlen(str_tail);
    if (sz_root + sz_tail + 1 > PATH_MAX) {
        return -ENAMETOOLONG;
    }
    memcpy(str_out, str_root, sz_root);
    memcpy(str_out + sz_root, str_tail, sz_tail + 1);
    return 0;
}

int64_t path_at(int i_dirfd, uint64_t u_path, char *str_out)
{
    char arr_raw[PATH_MAX];
    char arr_base[PATH_MAX];
    int64_t i_len = strncpy_from_user(arr_raw, u_path, sizeof arr_raw);

    if (i_len < 0) {
        return i_len;
    }
    if (i_len == 0) {
        return -ENOENT;
    }
    const char *str_base = "/";
    if (arr_raw[0] != '/') {
        if (i_dirfd == AT_FDCWD) {
            str_base = process_cwd(current_pid());
        } else {
            struct file *p_dir = process_fd_file(current_pid(), i_dirfd);
            if (p_dir == NULL) {
                return -EBADF;
            }
            bool b_dir = p_dir->e_type == FILE_TYPE_DIR;
            strncpy(arr_base, p_dir->arr_path, sizeof arr_base - 1);
            arr_base[sizeof arr_base - 1] = '\0';
            file_put(p_dir);
            if (!b_dir) {
                return -ENOTDIR;
            }
            str_base = arr_base;
        }
    }
    return under_root(process_root(current_pid()), str_base, arr_raw,
                      str_out);
}

/*
 * every directory on the way to the last name is one, and may be
 * searched: 0, or why not
 */
static int64_t walk_to(const char *str_path)
{
    char arr_prefix[PATH_MAX];
    struct vfs_file node;
    size_t sz_len = strlen(str_path);

    for (size_t sz_at = 1; sz_at < sz_len; sz_at++) {
        if (str_path[sz_at] != '/') {
            continue;
        }
        memcpy(arr_prefix, str_path, sz_at);
        arr_prefix[sz_at] = '\0';
        if (!vfs_open(arr_prefix, &node)) {
            return -ENOENT;
        }
        if (!node.is_dir) {
            return -ENOTDIR;
        }
        if (!may_access(&node, X_OK)) {
            return -EACCES;
        }
    }
    return 0;
}

int64_t path_lookup(const char *str_path, bool b_follow,
                    struct vfs_file *p_out)
{
    int64_t i_why = walk_to(str_path);
    if (i_why < 0) {
        return i_why;
    }
    bool b_found = b_follow ? vfs_open(str_path, p_out)
                            : vfs_open_nofollow(str_path, p_out);
    return b_found ? 0 : -ENOENT;
}

int64_t may_make(const char *str_path, bool b_must_be_new)
{
    char arr_dir[PATH_MAX];
    char arr_name[PATH_MAX];
    struct vfs_file node;

    if (path_is_root(str_path)
        || !path_split(str_path, arr_dir, sizeof arr_dir, arr_name,
                       sizeof arr_name)) {
        return -EEXIST;
    }
    int64_t i_why = path_lookup(arr_dir, true, &node);
    if (i_why < 0) {
        return i_why;
    }
    if (!node.is_dir) {
        return -ENOTDIR;
    }
    if (b_must_be_new && vfs_open_nofollow(str_path, &node)) {
        return -EEXIST;
    }
    vfs_open(arr_dir, &node);
    if (!may_access(&node, W_OK | X_OK)) {
        return -EACCES;
    }
    return vfs_writable(&node) ? 0 : -EROFS;
}

int64_t sys_mkdirat(int i_dirfd, uint64_t u_path, uint32_t u_mode)
{
    char arr_path[PATH_MAX];
    int64_t i_why = path_at(i_dirfd, u_path, arr_path);
    if (i_why == 0) {
        i_why = may_make(arr_path, true);
    }
    if (i_why < 0) {
        return i_why;
    }
    const struct process *p = process_find(current_pid());
    uint32_t u_umask = (p != NULL) ? p->u_umask : 022;
    return vfs_mkdir(arr_path, u_mode & ~u_umask & 07777,
                     (uint32_t)current_uid(), (uint32_t)current_gid())
           ? 0 : -ENOSPC;
}

/*
 * a device number as a program packs it, a major of twelve bits and a
 * minor of twenty split around it, made the kernel's, major << 8 | minor:
 * true, or false for a minor too wide to keep
 */
static bool rdev_from(uint32_t u_dev, uint32_t *p_rdev)
{
    uint32_t u_major = (u_dev & 0xfff00) >> 8;
    uint32_t u_minor = (u_dev & 0xff) | ((u_dev >> 12) & 0xfff00);
    *p_rdev = u_major << 8 | u_minor;
    return u_minor < 256;
}

/*
 * a file or a device, by the type in u_mode, 0 being a file. a device is
 * root's to make, since naming one is reaching it; a fifo or a socket is
 * refused, since nothing here opens one yet, and a directory is mkdir's
 */
int64_t sys_mknodat(int i_dirfd, uint64_t u_path, uint32_t u_mode,
                    uint32_t u_dev)
{
    char arr_path[PATH_MAX];
    uint32_t u_type = u_mode & S_IFMT;
    uint32_t u_rdev = 0;

    if (u_type == 0) {
        u_type = S_IFREG;
    }
    if (u_type != S_IFREG && u_type != S_IFCHR && u_type != S_IFBLK
        && u_type != S_IFIFO && u_type != S_IFSOCK && u_type != S_IFDIR) {
        return -EINVAL;
    }
    if (u_type == S_IFDIR || u_type == S_IFIFO || u_type == S_IFSOCK) {
        return -EPERM;
    }
    bool b_device = u_type != S_IFREG;
    if (b_device && current_uid() != 0) {
        return -EPERM;
    }
    if (b_device && !rdev_from(u_dev, &u_rdev)) {
        return -EINVAL;
    }
    int64_t i_why = path_at(i_dirfd, u_path, arr_path);
    if (i_why == 0) {
        i_why = may_make(arr_path, true);
    }
    if (i_why < 0) {
        return i_why;
    }
    const struct process *p = process_find(current_pid());
    uint32_t u_perms = u_mode & ~((p != NULL) ? p->u_umask : 022) & 07777;
    uint32_t u_uid = (uint32_t)current_uid();
    uint32_t u_gid = (uint32_t)current_gid();
    if (!b_device) {
        struct vfs_file node;
        return vfs_create(arr_path, u_perms, u_uid, u_gid, &node) ? 0
                                                                  : -ENOSPC;
    }
    /* a filesystem with nowhere to keep a device's number has none */
    return vfs_mknod(arr_path, u_type | u_perms, u_uid, u_gid, u_rdev)
           ? 0 : -EPERM;
}

int64_t sys_unlinkat(int i_dirfd, uint64_t u_path, int i_flags)
{
    char arr_path[PATH_MAX];
    struct vfs_file node;

    if (i_flags & ~AT_REMOVEDIR) {
        return -EINVAL;
    }
    int64_t i_why = path_at(i_dirfd, u_path, arr_path);
    if (i_why == 0) {
        i_why = path_lookup(arr_path, false, &node);
    }
    if (i_why < 0) {
        return i_why;
    }
    if (vfs_is_mount_point(arr_path)) {
        return -EBUSY;
    }
    i_why = may_make(arr_path, false);
    if (i_why < 0) {
        return i_why;
    }
    if (i_flags & AT_REMOVEDIR) {
        if (!node.is_dir) {
            return -ENOTDIR;
        }
        return vfs_rmdir(arr_path) ? 0 : -ENOTEMPTY;
    }
    if (node.is_dir) {
        return -EISDIR;
    }
    return vfs_unlink(arr_path) ? 0 : -EIO;
}

int64_t sys_renameat2(int i_olddir, uint64_t u_old, int i_newdir,
                      uint64_t u_new, uint32_t u_flags)
{
    char arr_from[PATH_MAX];
    char arr_to[PATH_MAX];
    struct vfs_file from;
    struct vfs_file to;

    /* RENAME_NOREPLACE, and nothing else the filesystems can promise */
    if (u_flags & ~1u) {
        return -EINVAL;
    }
    int64_t i_why = path_at(i_olddir, u_old, arr_from);
    if (i_why == 0) {
        i_why = path_at(i_newdir, u_new, arr_to);
    }
    if (i_why == 0) {
        i_why = path_lookup(arr_from, false, &from);
    }
    if (i_why < 0) {
        return i_why;
    }
    if (vfs_is_mount_point(arr_from) || vfs_is_mount_point(arr_to)) {
        return -EBUSY;
    }
    bool b_exists = path_lookup(arr_to, false, &to) == 0;
    if (b_exists && (u_flags & 1u)) {
        return -EEXIST;
    }
    if (b_exists && to.is_dir && !from.is_dir) {
        return -EISDIR;
    }
    if (b_exists && !to.is_dir && from.is_dir) {
        return -ENOTDIR;
    }
    if (strcmp(arr_from, arr_to) == 0) {
        return 0;
    }
    i_why = may_make(arr_from, false);
    if (i_why == 0) {
        i_why = may_make(arr_to, false);
    }
    if (i_why < 0) {
        return i_why;
    }
    return vfs_rename(arr_from, arr_to) ? 0 : -EXDEV;
}

int64_t sys_symlinkat(uint64_t u_target, int i_dirfd, uint64_t u_path)
{
    char arr_target[PATH_MAX];
    char arr_path[PATH_MAX];

    int64_t i_len = strncpy_from_user(arr_target, u_target, sizeof arr_target);
    if (i_len < 0) {
        return i_len;
    }
    if (i_len == 0) {
        return -ENOENT;
    }
    int64_t i_why = path_at(i_dirfd, u_path, arr_path);
    if (i_why == 0) {
        i_why = may_make(arr_path, true);
    }
    if (i_why < 0) {
        return i_why;
    }
    return vfs_symlink(arr_path, arr_target, (uint32_t)current_uid(),
                       (uint32_t)current_gid()) ? 0 : -EPERM;
}

int64_t sys_readlinkat(int i_dirfd, uint64_t u_path, uint64_t u_buf,
                       uint64_t u_size)
{
    char arr_path[PATH_MAX];
    char arr_target[PATH_MAX];
    struct vfs_file node;

    if ((int64_t)u_size <= 0) {
        return -EINVAL;
    }
    int64_t i_why = path_at(i_dirfd, u_path, arr_path);
    if (i_why == 0) {
        i_why = path_lookup(arr_path, false, &node);
    }
    if (i_why < 0) {
        return i_why;
    }
    if (!node.is_symlink || !vfs_readlink(arr_path, arr_target,
                                          sizeof arr_target)) {
        return -EINVAL;
    }
    /* the target, cut to fit, and no NUL: what readlink has always given */
    uint64_t u_len = strlen(arr_target);
    if (u_len > u_size) {
        u_len = u_size;
    }
    return (copy_to_user(u_buf, arr_target, u_len) == 0) ? (int64_t)u_len
                                                         : -EFAULT;
}

int64_t sys_linkat(int i_olddir, uint64_t u_old, int i_newdir,
                   uint64_t u_new, int i_flags)
{
    char arr_from[PATH_MAX];
    char arr_to[PATH_MAX];
    struct vfs_file node;

    if (i_flags & ~AT_SYMLINK_FOLLOW) {
        return -EINVAL;
    }
    int64_t i_why = path_at(i_olddir, u_old, arr_from);
    if (i_why == 0) {
        i_why = path_at(i_newdir, u_new, arr_to);
    }
    if (i_why == 0) {
        i_why = path_lookup(arr_from, (i_flags & AT_SYMLINK_FOLLOW) != 0,
                            &node);
    }
    if (i_why == 0) {
        i_why = may_make(arr_to, true);
    }
    if (i_why < 0) {
        return i_why;
    }
    /* a second name for one file is something no filesystem here makes */
    return -EPERM;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/open.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * opening, closing, and what can be done to a name without reading it.
 */

#include <velvet/syscalls.h>
#include <velvet/namei.h>
#include <velvet/errno.h>
#include <velvet/file.h>
#include <velvet/process.h>
#include <velvet/string.h>
#include <velvet/disk.h>
#include <asm/uaccess.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/stat.h>

/*
 * a file at the caller's lowest free descriptor, within its limit:
 * which, or -EMFILE with the file let go of
 */
int64_t install_fd(struct file *p_file, bool b_cloexec)
{
    int i_fd = process_fd_add(current_pid(), p_file, 0, b_cloexec);
    if (i_fd < 0) {
        file_put(p_file);
    }
    return i_fd;
}

/* what is at the path made, for an open with O_CREAT */
static int64_t make_file(const char *str_path, uint32_t u_mode,
                         struct vfs_file *p_node)
{
    int64_t i_why = may_make(str_path, true);
    if (i_why < 0) {
        return i_why;
    }
    const struct process *p = process_find(current_pid());
    uint32_t u_umask = (p != NULL) ? p->u_umask : 022;
    return vfs_create(str_path, u_mode & ~u_umask & 07777,
                      (uint32_t)current_uid(), (uint32_t)current_gid(),
                      p_node) ? 0 : -ENOSPC;
}

int64_t sys_openat(int i_dirfd, uint64_t u_path, int i_flags,
                   uint32_t u_mode)
{
    char arr_path[PATH_MAX];
    struct vfs_file node;
    int i_access = i_flags & O_ACCMODE;
    bool b_read = (i_flags & O_PATH) == 0 && i_access != O_WRONLY;
    bool b_write = (i_flags & O_PATH) == 0
                   && (i_access == O_WRONLY || i_access == O_RDWR);

    if (i_access == O_ACCMODE) {
        return -EINVAL;
    }
    int64_t i_why = path_at(i_dirfd, u_path, arr_path);
    if (i_why < 0) {
        return i_why;
    }
    i_why = path_lookup(arr_path, (i_flags & O_NOFOLLOW) == 0, &node);
    if (i_why == -ENOENT && (i_flags & O_CREAT)) {
        i_why = make_file(arr_path, u_mode, &node);
    } else if (i_why == 0 && (i_flags & O_CREAT) && (i_flags & O_EXCL)) {
        return -EEXIST;
    }
    if (i_why < 0) {
        return i_why;
    }

    if (node.is_symlink) {
        return -ELOOP;          /* O_NOFOLLOW, and a link at the end */
    }
    if ((i_flags & O_DIRECTORY) && !node.is_dir) {
        return -ENOTDIR;
    }
    if (node.is_dir && b_write) {
        return -EISDIR;
    }
    if ((b_read && !may_access(&node, R_OK))
        || ((b_write || (i_flags & O_TRUNC)) && !may_access(&node, W_OK))) {
        return -EACCES;
    }
    if ((b_write || (i_flags & O_TRUNC)) && !node.is_dir
        && node.kind != VFS_DEV && !vfs_writable(&node)) {
        return -EROFS;
    }
    if ((i_flags & O_TRUNC) && b_write && node.kind == VFS_DISK
        && node.size != 0 && !vfs_truncate(&node, 0)) {
        return -EIO;
    }

    uint32_t u_fmode = (b_read ? FILE_READ : 0) | (b_write ? FILE_WRITE : 0);
    /*
     * a device that will not open is one with nothing behind it, /dev/tty
     * for a process with no terminal; anything else is out of memory
     */
    struct file *p_file = vfs_open_file(&node, u_fmode);
    if (p_file == NULL) {
        return (node.kind == VFS_DEV) ? -ENXIO : -ENOMEM;
    }
    p_file->u_mode = u_fmode;
    p_file->u_flags = (uint32_t)i_flags & (O_APPEND | O_NONBLOCK);
    memcpy(p_file->arr_path, arr_path, sizeof p_file->arr_path);
    return install_fd(p_file, (i_flags & O_CLOEXEC) != 0);
}

int64_t sys_close(int i_fd)
{
    struct file *p_file = process_fd_take(current_pid(), i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    file_put(p_file);
    return 0;
}

int64_t sys_faccessat(int i_dirfd, uint64_t u_path, int i_mode, int i_flags)
{
    char arr_path[PATH_MAX];
    struct vfs_file node;

    if ((i_mode & ~(R_OK | W_OK | X_OK)) != 0
        || (i_flags & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) != 0) {
        return -EINVAL;
    }
    int64_t i_why = path_at(i_dirfd, u_path, arr_path);
    if (i_why == 0) {
        i_why = path_lookup(arr_path, (i_flags & AT_SYMLINK_NOFOLLOW) == 0,
                            &node);
    }
    if (i_why < 0 || i_mode == F_OK) {
        return i_why;
    }
    if ((i_mode & W_OK) && !node.is_dir && node.kind != VFS_DEV
        && !vfs_writable(&node)) {
        return -EROFS;
    }
    return may_access(&node, i_mode) ? 0 : -EACCES;
}

/* a directory the caller may search, as the place relative names start */
static int64_t change_dir(const char *str_path)
{
    struct vfs_file node;
    int64_t i_why = path_lookup(str_path, true, &node);
    if (i_why < 0) {
        return i_why;
    }
    if (!node.is_dir) {
        return -ENOTDIR;
    }
    if (!may_access(&node, X_OK)) {
        return -EACCES;
    }
    process_set_cwd(current_pid(), str_path);
    return 0;
}

int64_t sys_chdir(uint64_t u_path)
{
    char arr_path[PATH_MAX];
    int64_t i_why = path_at(AT_FDCWD, u_path, arr_path);
    return (i_why < 0) ? i_why : change_dir(arr_path);
}

int64_t sys_fchdir(int i_fd)
{
    char arr_path[PATH_MAX];
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    bool b_dir = p_file->e_type == FILE_TYPE_DIR;
    memcpy(arr_path, p_file->arr_path, sizeof arr_path);
    file_put(p_file);
    return b_dir ? change_dir(arr_path) : -ENOTDIR;
}

int64_t sys_getcwd(uint64_t u_buf, uint64_t u_size)
{
    const char *str_here = process_cwd(current_pid());
    uint64_t u_len = strlen(str_here) + 1;
    if (u_size < u_len) {
        return -ERANGE;
    }
    return (copy_to_user(u_buf, str_here, u_len) == 0) ? (int64_t)u_len
                                                       : -EFAULT;
}

/* only the owner, or root, may change what a file allows */
static int64_t change_mode(const char *str_path, uint32_t u_mode)
{
    struct vfs_file node;
    int64_t i_why = path_lookup(str_path, true, &node);
    if (i_why < 0) {
        return i_why;
    }
    int i_uid = current_uid();
    if (i_uid != 0 && (uint32_t)i_uid != node.uid) {
        return -EPERM;
    }
    if (!vfs_writable(&node)) {
        return -EROFS;
    }
    return vfs_chmod(str_path, u_mode & 07777) ? 0 : -EPERM;
}

int64_t sys_fchmodat(int i_dirfd, uint64_t u_path, uint32_t u_mode,
                     int i_flags)
{
    char arr_path[PATH_MAX];
    if (i_flags != 0) {
        return -EINVAL;
    }
    int64_t i_why = path_at(i_dirfd, u_path, arr_path);
    return (i_why < 0) ? i_why : change_mode(arr_path, u_mode);
}

/* the name a descriptor was opened by, when it was opened by one */
static int64_t fd_path(int i_fd, char *str_out)
{
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    bool b_named = p_file->arr_path[0] != '\0';
    memcpy(str_out, p_file->arr_path, PATH_MAX);
    file_put(p_file);
    return b_named ? 0 : -EINVAL;
}

int64_t sys_fchmod(int i_fd, uint32_t u_mode)
{
    char arr_path[PATH_MAX];
    int64_t i_why = fd_path(i_fd, arr_path);
    return (i_why < 0) ? i_why : change_mode(arr_path, u_mode);
}

/*
 * root's alone, since giving a file away is a way to escape a quota and
 * to plant one on somebody. -1 for either leaves it as it is
 */
static int64_t change_owner(const char *str_path, bool b_follow,
                            uint32_t u_uid, uint32_t u_gid)
{
    struct vfs_file node;
    int64_t i_why = path_lookup(str_path, b_follow, &node);
    if (i_why < 0) {
        return i_why;
    }
    if (current_uid() != 0) {
        return -EPERM;
    }
    if (!vfs_writable(&node)) {
        return -EROFS;
    }
    return vfs_chown(str_path, (u_uid == UINT32_MAX) ? node.uid : u_uid,
                     (u_gid == UINT32_MAX) ? node.gid : u_gid) ? 0 : -EPERM;
}

int64_t sys_fchownat(int i_dirfd, uint64_t u_path, uint32_t u_uid,
                     uint32_t u_gid, int i_flags)
{
    char arr_path[PATH_MAX];
    if (i_flags & ~AT_SYMLINK_NOFOLLOW) {
        return -EINVAL;
    }
    int64_t i_why = path_at(i_dirfd, u_path, arr_path);
    return (i_why < 0) ? i_why
           : change_owner(arr_path, (i_flags & AT_SYMLINK_NOFOLLOW) == 0,
                          u_uid, u_gid);
}

int64_t sys_fchown(int i_fd, uint32_t u_uid, uint32_t u_gid)
{
    char arr_path[PATH_MAX];
    int64_t i_why = fd_path(i_fd, arr_path);
    return (i_why < 0) ? i_why : change_owner(arr_path, true, u_uid, u_gid);
}

int64_t sys_truncate(uint64_t u_path, int64_t i_length)
{
    char arr_path[PATH_MAX];
    struct vfs_file node;

    if (i_length < 0) {
        return -EINVAL;
    }
    int64_t i_why = path_at(AT_FDCWD, u_path, arr_path);
    if (i_why == 0) {
        i_why = path_lookup(arr_path, true, &node);
    }
    if (i_why < 0) {
        return i_why;
    }
    if (node.is_dir) {
        return -EISDIR;
    }
    if (!may_access(&node, W_OK)) {
        return -EACCES;
    }
    if (!vfs_writable(&node)) {
        return -EROFS;
    }
    return vfs_truncate(&node, (uint64_t)i_length) ? 0 : -EFBIG;
}

int64_t sys_ftruncate(int i_fd, int64_t i_length)
{
    if (i_length < 0) {
        return -EINVAL;
    }
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    int64_t i_result = 0;
    if (p_file->e_type != FILE_TYPE_REGULAR
        || !(p_file->u_mode & FILE_WRITE)) {
        i_result = -EINVAL;
    } else if (!vfs_truncate(&p_file->node, (uint64_t)i_length)) {
        i_result = vfs_writable(&p_file->node) ? -EFBIG : -EROFS;
    }
    file_put(p_file);
    return i_result;
}

int64_t sys_umask(uint32_t u_mask)
{
    uint64_t u_flags;
    struct process *p = process_lock_slot(current_pid(), &u_flags);
    uint32_t u_was = 022;
    if (p != NULL) {
        u_was = p->u_umask;
        p->u_umask = u_mask & 0777;
    }
    process_unlock_slot(u_flags);
    return (int64_t)u_was;
}

int64_t sys_sync(void)
{
    if (disk_dirty()) {
        disk_sync();
    }
    return 0;
}

/*
 * one file is all of them: the disk is written out whole, which is more
 * than was asked for and never less
 */
int64_t sys_fsync(int i_fd)
{
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    file_put(p_file);
    if (disk_dirty() && !disk_sync()) {
        return -EIO;
    }
    return 0;
}

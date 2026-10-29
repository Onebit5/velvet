// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/stat.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a file is, and what a directory holds.
 */

#include <velvet/syscalls.h>
#include <velvet/namei.h>
#include <velvet/dev.h>
#include <velvet/errno.h>
#include <velvet/file.h>
#include <velvet/process.h>
#include <velvet/string.h>
#include <velvet/epoch.h>
#include <velvet/rtc.h>
#include <velvet/kmalloc.h>
#include <asm/uaccess.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/stat.h>
#include <uapi/velvet/dirent.h>

/* a device number as stat gives it */
#define DEVNO(major, minor) ((uint64_t)(major) << 8 | (minor))

/*
 * which filesystem a file is on: a disk's by the number of the drive or
 * partition it was mounted from, the one /dev gives that device, and
 * the ones kept in memory as anonymous ones, the way a filesystem with
 * no drive under it is numbered
 */
static uint64_t dev_of(const struct vfs_file *p_node)
{
    switch (p_node->kind) {
    case VFS_DISK:    return disk_dev(p_node->mount);
    case VFS_PROC:    return DEVNO(0, 2);
    case VFS_DEV:     return DEVNO(0, 3);
    case VFS_RAMFS:   return DEVNO(0, 16 + p_node->mount);
    default:          return 0;
    }
}

static int64_t seconds_of(const struct fat32_time *p_when)
{
    struct rtc_time date = {
        .second = p_when->second, .minute = p_when->minute,
        .hour = p_when->hour, .day = p_when->day, .month = p_when->month,
        .year = p_when->year,
    };
    return (p_when->year == 0) ? 0 : (int64_t)epoch_from_date(&date);
}

/*
 * what kind of thing a node is, as the type bits of a mode: its own,
 * where its filesystem keeps one, and otherwise what it is plainly
 */
static uint32_t type_bits(const struct vfs_file *p_node)
{
    if (p_node->u_type != 0) {
        return p_node->u_type;
    }
    if (p_node->is_dir) {
        return S_IFDIR;
    }
    if (p_node->is_symlink) {
        return S_IFLNK;
    }
    if (p_node->kind == VFS_DEV) {
        return dev_is_block(p_node->sz_dev) ? S_IFBLK : S_IFCHR;
    }
    return S_IFREG;
}

/* the stat of a file a name leads to */
static void fill_node(const struct vfs_file *p_node, const char *str_path,
                      struct stat *p_st)
{
    memset(p_st, 0, sizeof *p_st);
    uint32_t u_type = type_bits(p_node);
    p_st->st_dev = dev_of(p_node);
    p_st->st_ino = (p_node->u_ino != 0) ? p_node->u_ino : path_ino(str_path);
    p_st->st_nlink = p_node->is_dir ? 2 : 1;
    p_st->st_mode = u_type | (p_node->mode & 07777);
    if (u_type == S_IFCHR || u_type == S_IFBLK) {
        p_st->st_rdev = (p_node->kind == VFS_DEV) ? dev_rdev(p_node->sz_dev)
                                                  : p_node->u_rdev;
    }
    p_st->st_uid = p_node->uid;
    p_st->st_gid = p_node->gid;
    p_st->st_size = (int64_t)p_node->size;
    p_st->st_blksize = 4096;
    p_st->st_blocks = (int64_t)((p_node->size + 511) / 512);
    p_st->st_mtim.tv_sec = seconds_of(&p_node->written);
    p_st->st_atim = p_st->st_mtim;
    p_st->st_ctim = p_st->st_mtim;
}

/* and of one that has no name: a pipe, a terminal, a socket, a device */
static void fill_file(const struct file *p_file, struct stat *p_st)
{
    if (p_file->e_type == FILE_TYPE_REGULAR
        || p_file->e_type == FILE_TYPE_DIR) {
        struct vfs_file node = p_file->node;
        vfs_refresh(&node);
        fill_node(&node, p_file->arr_path, p_st);
        return;
    }
    memset(p_st, 0, sizeof *p_st);
    switch (p_file->e_type) {
    case FILE_TYPE_PIPE:
        p_st->st_mode = S_IFIFO | 0600;
        break;
    case FILE_TYPE_SOCKET:
        p_st->st_mode = S_IFSOCK | 0777;
        break;
    case FILE_TYPE_BLOCK:
        p_st->st_mode = S_IFBLK | DEV_BLOCK_MODE;
        p_st->st_rdev = p_file->u_rdev;
        p_st->st_dev = DEVNO(0, 3);
        break;
    default:
        p_st->st_mode = S_IFCHR | 0666;
        p_st->st_rdev = p_file->u_rdev;
        p_st->st_dev = DEVNO(0, 3);
        break;
    }
    p_st->st_ino = (uint64_t)(uintptr_t)p_file >> 4;
    p_st->st_nlink = 1;
    p_st->st_uid = (uint32_t)current_uid();
    p_st->st_gid = (uint32_t)current_gid();
    p_st->st_blksize = 4096;
}

int64_t sys_fstat(int i_fd, uint64_t u_stat)
{
    struct stat st;
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    fill_file(p_file, &st);
    file_put(p_file);
    return (copy_to_user(u_stat, &st, sizeof st) == 0) ? 0 : -EFAULT;
}

int64_t sys_newfstatat(int i_dirfd, uint64_t u_path, uint64_t u_stat,
                       int i_flags)
{
    char arr_path[PATH_MAX];
    struct vfs_file node;
    struct stat st;

    if (i_flags & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
        return -EINVAL;
    }
    /* an empty path with AT_EMPTY_PATH is the directory descriptor itself */
    char c_first = 1;
    int64_t i_why = 0;
    if ((i_flags & AT_EMPTY_PATH)
        && copy_from_user(&c_first, u_path, 1) == 0 && c_first == '\0') {
        if (i_dirfd != AT_FDCWD) {
            return sys_fstat(i_dirfd, u_stat);
        }
        strncpy(arr_path, process_cwd(current_pid()), PATH_MAX - 1);
        arr_path[PATH_MAX - 1] = '\0';
    } else {
        i_why = path_at(i_dirfd, u_path, arr_path);
    }
    if (i_why == 0) {
        i_why = path_lookup(arr_path, (i_flags & AT_SYMLINK_NOFOLLOW) == 0,
                            &node);
    }
    if (i_why < 0) {
        return i_why;
    }
    fill_node(&node, arr_path, &st);
    return (copy_to_user(u_stat, &st, sizeof st) == 0) ? 0 : -EFAULT;
}

/* where a record goes in a getdents64 buffer: its length, aligned to 8 */
static size_t record_len(size_t sz_name)
{
    return (offsetof(struct dirent64, d_name) + sz_name + 1 + 7) & ~(size_t)7;
}

/* a directory entry's type is the mode's, shifted down to its own byte */
static uint8_t type_of(const struct vfs_file *p_node)
{
    return (uint8_t)(type_bits(p_node) >> 12);
}

/*
 * as many entries as fit, from where the last call stopped: "." and ".."
 * first, then what the directory holds. the position counts entries,
 * which is what a d_off seeks back to
 */
int64_t sys_getdents64(int i_fd, uint64_t u_buf, uint64_t u_len)
{
    struct file *p_dir = process_fd_file(current_pid(), i_fd);
    if (p_dir == NULL) {
        return -EBADF;
    }
    if (p_dir->e_type != FILE_TYPE_DIR) {
        file_put(p_dir);
        return -ENOTDIR;
    }
    if (u_len > 65536) {
        u_len = 65536;
    }
    uint8_t *p_out = kmalloc(u_len + 1);
    if (p_out == NULL) {
        file_put(p_dir);
        return -ENOMEM;
    }

    size_t sz_used = 0;
    int64_t i_result = 0;
    for (;;) {
        uint64_t u_index = p_dir->u_pos;
        struct vfs_file entry;
        const char *str_name;
        char arr_child[PATH_MAX];

        if (u_index < 2) {
            str_name = (u_index == 0) ? "." : "..";
            memset(&entry, 0, sizeof entry);
            entry.is_dir = true;
            path_resolve(p_dir->arr_path, str_name, arr_child,
                         sizeof arr_child);
        } else if (vfs_readdir(p_dir->arr_path, (size_t)(u_index - 2),
                               &entry)) {
            str_name = entry.name;
            size_t sz_name = strlen(str_name);
            /* a directory's name sometimes comes with its slash on */
            if (sz_name > 1 && str_name[sz_name - 1] == '/') {
                entry.name[sz_name - 1] = '\0';
            }
            path_resolve(p_dir->arr_path, str_name, arr_child,
                         sizeof arr_child);
        } else {
            break;
        }

        size_t sz_name = strlen(str_name);
        size_t sz_rec = record_len(sz_name);
        if (sz_used + sz_rec > u_len) {
            if (sz_used == 0) {
                i_result = -EINVAL;
            }
            break;
        }
        struct dirent64 *p_rec = (struct dirent64 *)(p_out + sz_used);
        memset(p_rec, 0, sz_rec);
        p_rec->d_ino = (entry.u_ino != 0) ? entry.u_ino : path_ino(arr_child);
        p_rec->d_off = (int64_t)(u_index + 1);
        p_rec->d_reclen = (uint16_t)sz_rec;
        p_rec->d_type = type_of(&entry);
        memcpy(p_rec->d_name, str_name, sz_name + 1);
        sz_used += sz_rec;
        p_dir->u_pos = u_index + 1;
    }

    if (i_result == 0 && sz_used > 0
        && copy_to_user(u_buf, p_out, sz_used) != 0) {
        i_result = -EFAULT;
    }
    kfree(p_out);
    file_put(p_dir);
    return (i_result < 0) ? i_result : (int64_t)sz_used;
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/namespace.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the calls that change where filesystems hang, and the one that says
 * how much room is on one.
 */

#include <velvet/syscalls.h>
#include <velvet/namei.h>
#include <velvet/dev.h>
#include <velvet/disk.h>
#include <velvet/file.h>
#include <velvet/pmm.h>
#include <velvet/process.h>
#include <velvet/ramfs.h>
#include <velvet/string.h>
#include <velvet/vfs.h>
#include <asm/uaccess.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/mount.h>
#include <uapi/velvet/statfs.h>

#define TYPE_MAX    32
#define OPTIONS_MAX 64

/* the flags that change nothing, and are taken for that reason */
#define MS_HARMLESS (MS_SILENT | MS_NOATIME | MS_NODIRATIME | MS_RELATIME \
                     | MS_STRICTATIME)

/*
 * what a type mount is told about is, by the names programs use for
 * them. ext2, ext3 and ext4 are one driver, which reads whichever the
 * filesystem says it is, and tmpfs is ramfs by the other name it goes by
 */
struct fs_type {
    const char    *str_name;
    enum vfs_kind  e_kind;
    enum disk_kind e_disk;      /* for one on a drive, which driver */
};

static const struct fs_type s_arr_types[] = {
    { "ext4",     VFS_DISK,  DISK_EXT4 },
    { "ext3",     VFS_DISK,  DISK_EXT4 },
    { "ext2",     VFS_DISK,  DISK_EXT4 },
    { "vfat",     VFS_DISK,  DISK_FAT32 },
    { "ramfs",    VFS_RAMFS, DISK_NONE },
    { "tmpfs",    VFS_RAMFS, DISK_NONE },
    { "proc",     VFS_PROC,  DISK_NONE },
    { "devtmpfs", VFS_DEV,   DISK_NONE },
};

static const struct fs_type *type_named(const char *str_name)
{
    for (size_t sz_at = 0;
         sz_at < sizeof s_arr_types / sizeof s_arr_types[0]; sz_at++) {
        if (strcmp(s_arr_types[sz_at].str_name, str_name) == 0) {
            return &s_arr_types[sz_at];
        }
    }
    return NULL;
}

/* a string out of ring 3, or the empty one for none: 0, or why not */
static int64_t string_from(uint64_t u_from, char *str_out, size_t sz_size)
{
    str_out[0] = '\0';
    if (u_from == 0) {
        return 0;
    }
    int64_t i_len = strncpy_from_user(str_out, u_from, sz_size);
    return (i_len < 0) ? i_len : 0;
}

/*
 * the options a ramfs takes, mode=OCTAL for its root, in *p_mode: 0, or
 * -EINVAL for one it does not know
 */
static int64_t ramfs_options(char *str_options, uint32_t *p_mode)
{
    char *str_at = str_options;

    while (*str_at != '\0') {
        char *str_end = str_at;
        while (*str_end != '\0' && *str_end != ',') {
            str_end++;
        }
        char c_was = *str_end;
        *str_end = '\0';
        if (strncmp(str_at, "mode=", 5) != 0 || str_at[5] == '\0') {
            return -EINVAL;
        }
        uint32_t u_mode = 0;
        for (const char *p_digit = str_at + 5; *p_digit != '\0'; p_digit++) {
            if (*p_digit < '0' || *p_digit > '7' || u_mode > 07777) {
                return -EINVAL;
            }
            u_mode = u_mode * 8 + (uint32_t)(*p_digit - '0');
        }
        if (u_mode > 07777) {
            return -EINVAL;
        }
        *p_mode = u_mode;
        str_at = (c_was == ',') ? str_end + 1 : str_end;
    }
    return 0;
}

/* the drive or partition a name in /dev leads to, or why it does not */
static int64_t block_named(uint64_t u_source, const struct disk_block **pp_out)
{
    char arr_path[PATH_MAX];
    struct vfs_file node;

    if (u_source == 0) {
        return -ENOTBLK;
    }
    int64_t i_why = path_at(AT_FDCWD, u_source, arr_path);
    if (i_why == 0) {
        i_why = path_lookup(arr_path, true, &node);
    }
    if (i_why < 0) {
        return i_why;
    }
    if (node.kind != VFS_DEV || node.is_dir || !dev_is_block(node.sz_dev)) {
        return -ENOTBLK;
    }
    *pp_out = disk_block_find(dev_rdev(node.sz_dev));
    return (*pp_out != NULL) ? 0 : -ENXIO;
}

int64_t sys_mount(uint64_t u_source, uint64_t u_target, uint64_t u_type,
                  uint64_t u_flags, uint64_t u_data)
{
    char arr_target[PATH_MAX];
    char arr_type[TYPE_MAX];
    char arr_source[VFS_SOURCE_MAX];
    char arr_options[OPTIONS_MAX];
    struct vfs_file node;

    if (process_uid(current_pid()) != 0) {
        return -EPERM;
    }
    if ((u_flags & MS_MGC_MSK) == MS_MGC_VAL) {
        u_flags &= ~(uint64_t)MS_MGC_MSK;
    }
    if (u_flags & ~(uint64_t)MS_HARMLESS) {
        return -EINVAL;
    }
    int64_t i_why = path_at(AT_FDCWD, u_target, arr_target);
    if (i_why == 0) {
        i_why = path_lookup(arr_target, true, &node);
    }
    if (i_why == 0 && !node.is_dir) {
        i_why = -ENOTDIR;
    }
    if (i_why == 0) {
        i_why = string_from(u_type, arr_type, sizeof arr_type);
    }
    if (i_why == 0) {
        i_why = string_from(u_source, arr_source, sizeof arr_source);
    }
    if (i_why == 0) {
        i_why = string_from(u_data, arr_options, sizeof arr_options);
    }
    if (i_why < 0) {
        return i_why;
    }
    const struct fs_type *p_type = type_named(arr_type);
    if (p_type == NULL) {
        return -ENODEV;
    }
    /* only a filesystem in memory takes an option, its root's mode */
    uint32_t u_mode = 0755;
    if (p_type->e_kind == VFS_RAMFS) {
        i_why = ramfs_options(arr_options, &u_mode);
    } else if (arr_options[0] != '\0') {
        i_why = -EINVAL;
    }
    if (i_why < 0) {
        return i_why;
    }

    switch (p_type->e_kind) {
    case VFS_DISK: {
        const struct disk_block *p_block = NULL;
        i_why = block_named(u_source, &p_block);
        if (i_why < 0) {
            return i_why;
        }
        int i_slot = disk_mount_block(p_block, p_type->e_disk);
        if (i_slot < 0) {
            return i_slot;
        }
        i_why = vfs_mount_add(arr_target, VFS_DISK, (size_t)i_slot,
                              arr_source);
        if (i_why < 0) {
            disk_unmount_slot((size_t)i_slot);
        }
        return i_why;
    }
    case VFS_RAMFS: {
        int i_fs = ramfs_new(u_mode);
        if (i_fs < 0) {
            return -ENOMEM;
        }
        i_why = vfs_mount_add(arr_target, VFS_RAMFS, (size_t)i_fs,
                              arr_source);
        if (i_why < 0) {
            ramfs_free((size_t)i_fs);
        }
        return i_why;
    }
    default:
        /* /proc and /dev are the kernel's, the same wherever they hang */
        return vfs_mount_add(arr_target, p_type->e_kind, 0, arr_source);
    }
}

int64_t sys_umount2(uint64_t u_target, int i_flags)
{
    char arr_target[PATH_MAX];
    enum vfs_kind e_kind;
    size_t sz_slot;

    if (process_uid(current_pid()) != 0) {
        return -EPERM;
    }
    if (i_flags & ~UMOUNT_NOFOLLOW) {
        return -EINVAL;
    }
    int64_t i_why = path_at(AT_FDCWD, u_target, arr_target);
    if (i_why == 0) {
        i_why = vfs_mount_remove(arr_target, &e_kind, &sz_slot);
    }
    if (i_why < 0) {
        return i_why;
    }
    if (e_kind == VFS_DISK) {
        /*
         * off the table already, so a drive that refuses the last of
         * what was held for it has lost that much, and says so
         */
        return disk_unmount_slot(sz_slot) ? 0 : -EIO;
    }
    if (e_kind == VFS_RAMFS) {
        ramfs_free(sz_slot);
    }
    return 0;
}



/* how much room the filesystem a node is on has, as statfs says it */
static void space_of(const struct vfs_file *p_node, struct statfs *p_st)
{
    memset(p_st, 0, sizeof *p_st);
    p_st->f_bsize = PAGE_SIZE;
    p_st->f_namelen = 255;
    p_st->f_fsid[0] = (int32_t)p_node->kind;
    p_st->f_fsid[1] = (int32_t)p_node->mount;

    switch (p_node->kind) {
    case VFS_DISK: {
        uint64_t u_used = 0;
        uint64_t u_total = 0;
        uint32_t u_block = disk_cluster_bytes(p_node->mount);
        p_st->f_type = (disk_which(p_node->mount) == DISK_EXT4)
                       ? EXT4_SUPER_MAGIC : MSDOS_SUPER_MAGIC;
        if (u_block != 0 && disk_usage(p_node->mount, &u_used, &u_total)) {
            p_st->f_bsize = u_block;
            p_st->f_blocks = u_total / u_block;
            p_st->f_bfree = (u_total - u_used) / u_block;
            p_st->f_bavail = p_st->f_bfree;
        }
        disk_files(p_node->mount, &p_st->f_files, &p_st->f_ffree);
        break;
    }
    case VFS_RAMFS: {
        /* as much as the machine has left, and what it holds already */
        uint64_t u_bytes = 0;
        uint64_t u_nodes = 0;
        ramfs_usage(p_node->mount, &u_bytes, &u_nodes);
        p_st->f_type = RAMFS_MAGIC;
        p_st->f_bfree = pmm_free_bytes() / PAGE_SIZE;
        p_st->f_bavail = p_st->f_bfree;
        p_st->f_blocks = u_bytes / PAGE_SIZE + p_st->f_bfree;
        p_st->f_ffree = ramfs_room();
        p_st->f_files = u_nodes + p_st->f_ffree;
        p_st->f_namelen = RAMFS_NAME_MAX - 1;
        break;
    }
    case VFS_RAMDISK:
        p_st->f_type = RAMFS_MAGIC;
        p_st->f_flags = ST_RDONLY;
        break;
    case VFS_PROC:
        p_st->f_type = PROC_SUPER_MAGIC;
        break;
    default:
        p_st->f_type = TMPFS_MAGIC;
        break;
    }
    p_st->f_frsize = p_st->f_bsize;
}

int64_t sys_statfs(uint64_t u_path, uint64_t u_buf)
{
    char arr_path[PATH_MAX];
    struct vfs_file node;
    struct statfs st;

    int64_t i_why = path_at(AT_FDCWD, u_path, arr_path);
    if (i_why == 0) {
        i_why = path_lookup(arr_path, true, &node);
    }
    if (i_why < 0) {
        return i_why;
    }
    space_of(&node, &st);
    return (copy_to_user(u_buf, &st, sizeof st) == 0) ? 0 : -EFAULT;
}

int64_t sys_fstatfs(int i_fd, uint64_t u_buf)
{
    struct statfs st;
    struct file *p_file = process_fd_file(current_pid(), i_fd);

    if (p_file == NULL) {
        return -EBADF;
    }
    if (p_file->e_type == FILE_TYPE_REGULAR
        || p_file->e_type == FILE_TYPE_DIR) {
        space_of(&p_file->node, &st);
    } else {
        /* a file with no name is on a filesystem of what it is */
        memset(&st, 0, sizeof st);
        st.f_type = (p_file->e_type == FILE_TYPE_PIPE) ? PIPEFS_MAGIC
                  : (p_file->e_type == FILE_TYPE_SOCKET) ? SOCKFS_MAGIC
                  : TMPFS_MAGIC;
        st.f_bsize = PAGE_SIZE;
        st.f_frsize = PAGE_SIZE;
        st.f_namelen = 255;
    }
    file_put(p_file);
    return (copy_to_user(u_buf, &st, sizeof st) == 0) ? 0 : -EFAULT;
}

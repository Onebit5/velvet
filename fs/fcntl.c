// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/fcntl.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * descriptors: copied, asked about, set, and pipes made of them.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/file.h>
#include <velvet/pipe.h>
#include <velvet/process.h>
#include <asm/uaccess.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/termios.h>
#include <uapi/velvet/ext4.h>
#include <velvet/disk.h>
#include <velvet/ext4.h>
#include <velvet/kprintf.h>

/* another descriptor on fd's file, the lowest free from i_from up */
static int64_t dup_from(int i_fd, int i_from, bool b_cloexec)
{
    if (i_from < 0 || i_from >= process_fd_limit(current_pid())) {
        return -EINVAL;
    }
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    int i_new = process_fd_add(current_pid(), p_file, i_from, b_cloexec);
    if (i_new < 0) {
        file_put(p_file);
    }
    return i_new;
}

int64_t sys_dup(int i_fd)
{
    return dup_from(i_fd, 0, false);
}

int64_t sys_dup3(int i_from, int i_to, int i_flags)
{
    if (i_from == i_to || (i_flags & ~O_CLOEXEC) != 0) {
        return -EINVAL;
    }
    if (i_to < 0 || i_to >= process_fd_limit(current_pid())) {
        return -EBADF;
    }
    struct file *p_file = process_fd_file(current_pid(), i_from);
    if (p_file == NULL) {
        return -EBADF;
    }
    struct file *p_old = NULL;
    process_fd_replace(current_pid(), i_to, p_file,
                       (i_flags & O_CLOEXEC) != 0, &p_old);
    file_put(p_old);
    return i_to;
}

int64_t sys_dup2(int i_from, int i_to)
{
    if (i_from == i_to) {
        struct file *p_file = process_fd_file(current_pid(), i_from);
        if (p_file == NULL) {
            return -EBADF;
        }
        file_put(p_file);
        return i_to;
    }
    return sys_dup3(i_from, i_to, 0);
}

int64_t sys_fcntl(int i_fd, int i_cmd, uint64_t u_arg)
{
    int i_pid = current_pid();

    switch (i_cmd) {
    case F_DUPFD:
        return dup_from(i_fd, (int)u_arg, false);
    case F_DUPFD_CLOEXEC:
        return dup_from(i_fd, (int)u_arg, true);
    case F_GETFD: {
        int i_bit = process_fd_cloexec(i_pid, i_fd, -1);
        return (i_bit < 0) ? i_bit : (i_bit ? FD_CLOEXEC : 0);
    }
    case F_SETFD: {
        int i_bit = process_fd_cloexec(i_pid, i_fd,
                                       (u_arg & FD_CLOEXEC) ? 1 : 0);
        return (i_bit < 0) ? i_bit : 0;
    }
    default:
        break;
    }

    struct file *p_file = process_fd_file(i_pid, i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    int64_t i_result;
    switch (i_cmd) {
    case F_GETFL: {
        int i_access = (p_file->u_mode == (FILE_READ | FILE_WRITE)) ? O_RDWR
                     : (p_file->u_mode == FILE_WRITE) ? O_WRONLY
                     : O_RDONLY;
        i_result = i_access | (int)p_file->u_flags;
        break;
    }
    case F_SETFL:
        /* what an open file may change about itself, and nothing else */
        p_file->u_flags = (uint32_t)u_arg & (O_APPEND | O_NONBLOCK);
        i_result = 0;
        break;
    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        /*
         * nothing here keeps locks. saying one was granted would let two
         * programs both believe they hold it
         */
        i_result = -ENOLCK;
        break;
    default:
        i_result = -EINVAL;
        break;
    }
    file_put(p_file);
    return i_result;
}

int64_t sys_pipe2(uint64_t u_fds, int i_flags)
{
    if (i_flags & ~(O_CLOEXEC | O_NONBLOCK)) {
        return -EINVAL;
    }
    struct file *p_read = NULL;
    struct file *p_write = NULL;
    if (!pipe_open(&p_read, &p_write)) {
        return -ENFILE;
    }
    p_read->u_flags = (uint32_t)i_flags & O_NONBLOCK;
    p_write->u_flags = (uint32_t)i_flags & O_NONBLOCK;

    bool b_cloexec = (i_flags & O_CLOEXEC) != 0;
    int64_t i_read = install_fd(p_read, b_cloexec);
    if (i_read < 0) {
        file_put(p_write);
        return i_read;
    }
    int64_t i_write = install_fd(p_write, b_cloexec);
    if (i_write < 0) {
        sys_close((int)i_read);
        return i_write;
    }
    int32_t arr_fds[2] = { (int32_t)i_read, (int32_t)i_write };
    if (copy_to_user(u_fds, arr_fds, sizeof arr_fds) != 0) {
        sys_close((int)i_read);
        sys_close((int)i_write);
        return -EFAULT;
    }
    return 0;
}

/*
 * the filesystem a descriptor is open on grown to u_arg's count of its
 * own blocks, or as far as its partition and its own tables allow when
 * that is 0. root's alone, and only an ext4 one of the disk's. why one
 * cannot is said in the kernel's log, since -EINVAL says only that
 */
static int64_t resize_fs(const struct file *p_file, uint64_t u_arg)
{
    uint64_t u_blocks = 0;
    if (p_file->node.kind != VFS_DISK
        || (p_file->e_type != FILE_TYPE_REGULAR
            && p_file->e_type != FILE_TYPE_DIR)) {
        return -ENOTTY;
    }
    if (copy_from_user(&u_blocks, u_arg, sizeof u_blocks) != 0) {
        return -EFAULT;
    }
    if (process_uid(current_pid()) != 0) {
        return -EPERM;
    }
    size_t sz_which = p_file->node.mount;
    if (u_blocks == 0) {
        uint64_t u_room = disk_room(sz_which);
        uint64_t u_ceiling = disk_ceiling(sz_which);
        u_blocks = (u_room < u_ceiling) ? u_room : u_ceiling;
    }
    struct ext4_resize result;
    const char *str_why = NULL;
    if (!disk_resize(sz_which, u_blocks, &result, &str_why)) {
        kprintf("[ext4] %s: not resized: %s\n",
                disk_mount_point(sz_which),
                (str_why != NULL) ? str_why : "it would not resize");
        return -EINVAL;
    }
    return 0;
}

/*
 * the few requests any descriptor understands, and the rest handed to
 * the file's own, which a file that has none answers by not being a
 * terminal
 */
int64_t sys_ioctl(int i_fd, uint32_t u_cmd, uint64_t u_arg)
{
    if (u_cmd == FIOCLEX || u_cmd == FIONCLEX) {
        int i_bit = process_fd_cloexec(current_pid(), i_fd,
                                       u_cmd == FIOCLEX ? 1 : 0);
        return (i_bit < 0) ? i_bit : 0;
    }
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    int64_t i_result;
    if (u_cmd == EXT4_IOC_RESIZE_FS) {
        i_result = resize_fs(p_file, u_arg);
    } else if (u_cmd == FIONBIO) {
        int32_t i_on = 0;
        i_result = (copy_from_user(&i_on, u_arg, sizeof i_on) == 0) ? 0
                                                                    : -EFAULT;
        if (i_result == 0) {
            if (i_on) {
                p_file->u_flags |= O_NONBLOCK;
            } else {
                p_file->u_flags &= ~(uint32_t)O_NONBLOCK;
            }
        }
    } else if (p_file->p_ops->ioctl != NULL) {
        i_result = p_file->p_ops->ioctl(p_file, u_cmd, u_arg);
    } else {
        i_result = -ENOTTY;
    }
    file_put(p_file);
    return i_result;
}

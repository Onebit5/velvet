// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/read_write.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * read and write, in every shape a program asks for them.
 */

#include <velvet/syscalls.h>
#include <velvet/errno.h>
#include <velvet/file.h>
#include <velvet/process.h>
#include <velvet/kmalloc.h>
#include <asm/uaccess.h>
#include <uapi/velvet/fcntl.h>
#include <uapi/velvet/uio.h>

/* how much goes through the kernel's own buffer at a time */
#define CHUNK 4096

/*
 * a file the caller has open for i_mode, with a reference to put: or
 * NULL and why not in *p_why
 */
static struct file *file_for(int i_fd, uint32_t u_mode, int64_t *p_why)
{
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        *p_why = -EBADF;
        return NULL;
    }
    if (!(p_file->u_mode & u_mode)) {
        file_put(p_file);
        *p_why = -EBADF;
        return NULL;
    }
    return p_file;
}

/*
 * a signal while waiting: the call is started again after it unless a
 * handler without SA_RESTART runs, the same answer any other wait gives
 */
static int64_t restartable(int64_t i_got)
{
    return (i_got == -EINTR) ? -ERESTARTSYS : i_got;
}

/*
 * a file with a position, a regular one, a drive, a directory, has an end
 * and is read until it is reached; anything else answers with what it
 * has, and has no offset to be read at
 */
static bool has_position(const struct file *p_file)
{
    return p_file->p_ops->seek != NULL;
}

/*
 * up to u_len bytes into ring 3 at u_buf, a chunk at a time, until the
 * caller has what it asked for or a file with a position ends
 */
static int64_t read_into(struct file *p_file, uint64_t u_buf, uint64_t u_len)
{
    if (p_file->p_ops->read == NULL) {
        return -EINVAL;
    }
    if (u_len == 0) {
        return 0;
    }
    if (!access_ok(u_buf, u_len)) {
        return -EFAULT;
    }
    char *p_buf = kmalloc(CHUNK);
    if (p_buf == NULL) {
        return -ENOMEM;
    }
    uint64_t u_done = 0;
    int64_t i_result = 0;
    while (u_done < u_len) {
        uint64_t u_want = (u_len - u_done < CHUNK) ? u_len - u_done : CHUNK;
        int64_t i_got = p_file->p_ops->read(p_file, p_buf, u_want);
        if (i_got <= 0) {
            i_result = i_got;
            break;
        }
        if (copy_to_user(u_buf + u_done, p_buf, (size_t)i_got) != 0) {
            i_result = -EFAULT;
            break;
        }
        u_done += (uint64_t)i_got;
        if (!has_position(p_file) || (uint64_t)i_got < u_want) {
            break;
        }
    }
    kfree(p_buf);
    return (u_done > 0) ? (int64_t)u_done : restartable(i_result);
}

/*
 * all of u_len out of ring 3, a chunk at a time, stopping short only
 * when a chunk did. a pipe nobody reads is SIGPIPE as well as -EPIPE
 */
static int64_t write_from(struct file *p_file, uint64_t u_buf,
                          uint64_t u_len)
{
    if (p_file->p_ops->write == NULL) {
        return -EINVAL;
    }
    if (u_len == 0) {
        return 0;
    }
    if (!access_ok(u_buf, u_len)) {
        return -EFAULT;
    }
    char *p_buf = kmalloc(CHUNK);
    if (p_buf == NULL) {
        return -ENOMEM;
    }
    uint64_t u_done = 0;
    int64_t i_result = 0;
    while (u_done < u_len) {
        uint64_t u_want = (u_len - u_done < CHUNK) ? u_len - u_done : CHUNK;
        if (copy_from_user(p_buf, u_buf + u_done, u_want) != 0) {
            i_result = -EFAULT;
            break;
        }
        int64_t i_put = p_file->p_ops->write(p_file, p_buf, u_want);
        if (i_put <= 0) {
            i_result = i_put;
            break;
        }
        u_done += (uint64_t)i_put;
        if ((uint64_t)i_put < u_want) {
            break;
        }
    }
    kfree(p_buf);
    if (u_done > 0) {
        return (int64_t)u_done;
    }
    if (i_result == -EPIPE) {
        process_signal(current_pid(), SIGPIPE);
    }
    return restartable(i_result);
}

int64_t sys_read(int i_fd, uint64_t u_buf, uint64_t u_len)
{
    int64_t i_why = 0;
    struct file *p_file = file_for(i_fd, FILE_READ, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    int64_t i_result = read_into(p_file, u_buf, u_len);
    file_put(p_file);
    return i_result;
}

int64_t sys_write(int i_fd, uint64_t u_buf, uint64_t u_len)
{
    int64_t i_why = 0;
    struct file *p_file = file_for(i_fd, FILE_WRITE, &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    int64_t i_result = write_from(p_file, u_buf, u_len);
    file_put(p_file);
    return i_result;
}

/* at an offset, and the file's own position left where it was */
static int64_t at_offset(int i_fd, uint64_t u_buf, uint64_t u_len,
                         int64_t i_offset, bool b_write)
{
    int64_t i_why = 0;
    struct file *p_file = file_for(i_fd, b_write ? FILE_WRITE : FILE_READ,
                                   &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    int64_t i_result;
    if (!has_position(p_file)) {
        i_result = -ESPIPE;
    } else if (i_offset < 0) {
        i_result = -EINVAL;
    } else {
        uint64_t u_was = p_file->u_pos;
        uint32_t u_flags = p_file->u_flags;
        p_file->u_pos = (uint64_t)i_offset;
        p_file->u_flags &= ~(uint32_t)O_APPEND;
        i_result = b_write ? write_from(p_file, u_buf, u_len)
                           : read_into(p_file, u_buf, u_len);
        p_file->u_flags = u_flags;
        p_file->u_pos = u_was;
    }
    file_put(p_file);
    return i_result;
}

int64_t sys_pread64(int i_fd, uint64_t u_buf, uint64_t u_len,
                    int64_t i_offset)
{
    return at_offset(i_fd, u_buf, u_len, i_offset, false);
}

int64_t sys_pwrite64(int i_fd, uint64_t u_buf, uint64_t u_len,
                     int64_t i_offset)
{
    return at_offset(i_fd, u_buf, u_len, i_offset, true);
}

/* each piece in turn, until one comes up short */
static int64_t vectored(int i_fd, uint64_t u_iov, int i_count, bool b_write)
{
    if (i_count < 0 || i_count > IOV_MAX) {
        return -EINVAL;
    }
    int64_t i_why = 0;
    struct file *p_file = file_for(i_fd, b_write ? FILE_WRITE : FILE_READ,
                                   &i_why);
    if (p_file == NULL) {
        return i_why;
    }
    int64_t i_done = 0;
    int64_t i_result = 0;
    for (int i_at = 0; i_at < i_count; i_at++) {
        struct iovec piece;
        if (copy_from_user(&piece, u_iov + (uint64_t)i_at * sizeof piece,
                           sizeof piece) != 0) {
            i_result = -EFAULT;
            break;
        }
        if (piece.iov_len == 0) {
            continue;
        }
        int64_t i_moved = b_write
                          ? write_from(p_file, piece.iov_base, piece.iov_len)
                          : read_into(p_file, piece.iov_base, piece.iov_len);
        if (i_moved < 0) {
            i_result = i_moved;
            break;
        }
        i_done += i_moved;
        if ((uint64_t)i_moved < piece.iov_len) {
            break;
        }
    }
    file_put(p_file);
    return (i_done > 0) ? i_done : i_result;
}

int64_t sys_readv(int i_fd, uint64_t u_iov, int i_count)
{
    return vectored(i_fd, u_iov, i_count, false);
}

int64_t sys_writev(int i_fd, uint64_t u_iov, int i_count)
{
    return vectored(i_fd, u_iov, i_count, true);
}

int64_t sys_lseek(int i_fd, int64_t i_offset, int i_whence)
{
    struct file *p_file = process_fd_file(current_pid(), i_fd);
    if (p_file == NULL) {
        return -EBADF;
    }
    int64_t i_to = -ESPIPE;
    if (i_whence < SEEK_SET || i_whence > SEEK_END) {
        i_to = -EINVAL;
    } else if (p_file->p_ops->seek != NULL) {
        i_to = p_file->p_ops->seek(p_file, i_offset, i_whence);
    }
    file_put(p_file);
    return i_to;
}

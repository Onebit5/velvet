// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/file.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * an open file: what a descriptor points at, shared by every one that does.
 */

#ifndef VELVET_FILE_H
#define VELVET_FILE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <velvet/vfs.h>
#include <velvet/path.h>

/* what a file was opened for, which no later call can widen */
#define FILE_READ  1u
#define FILE_WRITE 2u

/*
 * what kind of thing is open. a regular file has an end and is read until
 * it is reached; the others answer with what they have
 */
enum file_type {
    FILE_TYPE_REGULAR,
    FILE_TYPE_PIPE,
    FILE_TYPE_TTY,
    FILE_TYPE_CHAR,             /* a device in /dev */
    FILE_TYPE_DIR,              /* read with getdents64, not read */
    FILE_TYPE_SOCKET,
};

struct file;

/*
 * what can be done to an open file, filled in by whoever opens one. each
 * takes and gives the kernel's own memory and answers with a count, or
 * minus an errno. one left NULL is a thing that kind of file cannot do
 */
struct file_operations {
    /* from the file's position, or from wherever it reads */
    int64_t (*read)(struct file *p_file, void *p_buf, size_t sz_len);
    int64_t (*write)(struct file *p_file, const void *p_buf, size_t sz_len);
    /* SEEK_SET, SEEK_CUR or SEEK_END: the new position */
    int64_t (*seek)(struct file *p_file, int64_t i_offset, int i_whence);
    /* the last descriptor on it, in any process, has closed */
    void (*release)(struct file *p_file);
    /* what a read or a write would do without waiting: POLLIN and the rest */
    uint32_t (*poll)(struct file *p_file);
    /* a request only this kind of file understands, u_arg as ring 3's */
    int64_t (*ioctl)(struct file *p_file, uint32_t u_cmd, uint64_t u_arg);
};

/*
 * one per open, not one per descriptor. dup and fork hand out another
 * descriptor on the same file, so the two share its position, which is
 * what lets a shell's child write after what its parent wrote
 */
struct file {
    const struct file_operations *p_ops;
    uint32_t u_refs;            /* descriptors pointing here, anywhere */
    uint32_t u_mode;            /* FILE_READ and FILE_WRITE */
    uint32_t u_flags;           /* O_APPEND and O_NONBLOCK, which fcntl sets */
    enum file_type e_type;
    uint32_t u_rdev;            /* a device's number, major << 8 | minor */
    uint64_t u_pos;

    struct vfs_file node;       /* a file on a filesystem: where it lives */
    char arr_path[PATH_MAX];    /* the name it was opened by, made absolute */
    void *p_private;            /* the pipe, for a pipe's end */
};

/*
 * a new file with one reference, which the caller hands to a descriptor
 * or puts. NULL when there is no memory
 */
struct file *file_alloc(const struct file_operations *p_ops, uint32_t u_mode,
                        enum file_type e_type);

void file_get(struct file *p_file);

/*
 * a file put at the caller's lowest free descriptor below its limit,
 * keeping the reference: which, or -EMFILE with the file let go of
 */
int64_t install_fd(struct file *p_file, bool b_cloexec);

/*
 * one reference fewer, released with the last. never with a lock held:
 * releasing a pipe's end wakes whoever waits at the other one
 */
void file_put(struct file *p_file);

#endif

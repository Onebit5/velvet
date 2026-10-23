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
    enum file_type e_type;
    uint64_t u_pos;

    struct vfs_file node;       /* a file on a filesystem: where it lives */
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
 * one reference fewer, released with the last. never with a lock held:
 * releasing a pipe's end wakes whoever waits at the other one
 */
void file_put(struct file *p_file);

#endif

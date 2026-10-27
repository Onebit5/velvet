// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/dev.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /dev: nothing, zeroes, a full disk, randomness, the terminals, and the
 * drives with their partitions.
 */

#include <velvet/dev.h>
#include <velvet/blockdev.h>
#include <velvet/disk.h>
#include <velvet/file.h>
#include <velvet/random.h>
#include <velvet/string.h>
#include <velvet/tty.h>
#include <uapi/velvet/errno.h>

/* null: reads are the end, and writes go nowhere and succeed */
static int64_t null_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    (void)p_file;
    (void)p_buf;
    (void)sz_len;
    return 0;
}

static int64_t sink_write(struct file *p_file, const void *p_buf,
                          size_t sz_len)
{
    (void)p_file;
    (void)p_buf;
    return (int64_t)sz_len;
}

/* zero: as many zeroes as asked for, for ever */
static int64_t zero_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    (void)p_file;
    memset(p_buf, 0, sz_len);
    return (int64_t)sz_len;
}

/* full: zeroes to read, and no room to write, for testing what does */
static int64_t full_write(struct file *p_file, const void *p_buf,
                          size_t sz_len)
{
    (void)p_file;
    (void)p_buf;
    (void)sz_len;
    return -ENOSPC;
}

/*
 * random and urandom: the same generator, which never runs dry once
 * seeded. what a program writes is stirred in and cannot make the
 * output any more predictable than it was
 */
static int64_t random_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    (void)p_file;
    random_bytes(p_buf, sz_len);
    return (int64_t)sz_len;
}

static int64_t random_write(struct file *p_file, const void *p_buf,
                            size_t sz_len)
{
    (void)p_file;
    random_mix(p_buf, sz_len);
    return (int64_t)sz_len;
}

static const struct file_operations null_ops = {
    .read = null_read,
    .write = sink_write,
};

static const struct file_operations zero_ops = {
    .read = zero_read,
    .write = sink_write,
};

static const struct file_operations full_ops = {
    .read = zero_read,
    .write = full_write,
};

static const struct file_operations random_ops = {
    .read = random_read,
    .write = random_write,
};

/*
 * the terminals are tty.c's own. /dev/ttyN is console N's, from 1 as the
 * consoles are numbered on the keyboard, /dev/console is the first, the
 * one the kernel speaks on, and /dev/tty is whichever is the caller's
 */
#define DEV_TTY_OWN (-1)
#define DEV_NOT_TTY (-2)

struct dev_entry {
    const char *str_name;
    uint32_t u_mode;
    uint32_t u_rdev;            /* major << 8 | minor */
    const struct file_operations *p_ops;
    int i_console;              /* a terminal's console, or one of the above */
};

#define DEVNO(major, minor) ((uint32_t)(major) << 8 | (minor))

static const struct dev_entry s_arr_devs[] = {
    { "null",    0666, DEVNO(1, 3), &null_ops,   DEV_NOT_TTY },
    { "zero",    0666, DEVNO(1, 5), &zero_ops,   DEV_NOT_TTY },
    { "full",    0666, DEVNO(1, 7), &full_ops,   DEV_NOT_TTY },
    { "random",  0666, DEVNO(1, 8), &random_ops, DEV_NOT_TTY },
    { "urandom", 0666, DEVNO(1, 9), &random_ops, DEV_NOT_TTY },
    { "tty",     0666, DEVNO(5, 0), NULL,        DEV_TTY_OWN },
    { "console", 0600, DEVNO(5, 1), NULL,        0 },
    { "tty1",    0620, DEVNO(TTY_MAJOR, 1), NULL, 0 },
    { "tty2",    0620, DEVNO(TTY_MAJOR, 2), NULL, 1 },
    { "tty3",    0620, DEVNO(TTY_MAJOR, 3), NULL, 2 },
    { "tty4",    0620, DEVNO(TTY_MAJOR, 4), NULL, 3 },
    { "tty5",    0620, DEVNO(TTY_MAJOR, 5), NULL, 4 },
};

#define DEV_COUNT (sizeof s_arr_devs / sizeof s_arr_devs[0])

/* and after them, the drives and partitions the disk layer found */
static const struct disk_block *block_at(size_t sz_index)
{
    return (sz_index >= DEV_COUNT) ? disk_block_at(sz_index - DEV_COUNT)
                                   : NULL;
}

size_t dev_count(void)
{
    return DEV_COUNT + disk_block_count();
}

const char *dev_name(size_t sz_index)
{
    const struct disk_block *p_block = block_at(sz_index);
    if (p_block != NULL) {
        return p_block->arr_name;
    }
    return (sz_index < DEV_COUNT) ? s_arr_devs[sz_index].str_name : NULL;
}

uint32_t dev_mode(size_t sz_index)
{
    if (block_at(sz_index) != NULL) {
        return DEV_BLOCK_MODE;
    }
    return (sz_index < DEV_COUNT) ? s_arr_devs[sz_index].u_mode : 0;
}

uint32_t dev_rdev(size_t sz_index)
{
    const struct disk_block *p_block = block_at(sz_index);
    if (p_block != NULL) {
        return p_block->u_rdev;
    }
    return (sz_index < DEV_COUNT) ? s_arr_devs[sz_index].u_rdev : 0;
}

bool dev_is_block(size_t sz_index)
{
    return block_at(sz_index) != NULL;
}

bool dev_find(const char *str_name, size_t *p_index)
{
    size_t sz_count = dev_count();
    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        if (strcmp(dev_name(sz_at), str_name) == 0) {
            *p_index = sz_at;
            return true;
        }
    }
    return false;
}

struct file *dev_open(size_t sz_index, uint32_t u_mode)
{
    const struct disk_block *p_block = block_at(sz_index);
    if (p_block != NULL) {
        return blockdev_open(p_block->u_rdev, u_mode);
    }
    if (sz_index >= DEV_COUNT) {
        return NULL;
    }
    const struct dev_entry *p_dev = &s_arr_devs[sz_index];
    if (p_dev->i_console == DEV_TTY_OWN) {
        return tty_open_own(u_mode);
    }
    if (p_dev->i_console >= 0) {
        return tty_open((unsigned)p_dev->i_console, u_mode);
    }
    struct file *p_file = file_alloc(p_dev->p_ops, u_mode, FILE_TYPE_CHAR);
    if (p_file != NULL) {
        p_file->u_rdev = p_dev->u_rdev;
    }
    return p_file;
}

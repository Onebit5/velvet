// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/dev.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /dev: nothing, zeroes, a full disk, randomness and the terminal.
 */

#include <velvet/dev.h>
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
 * the terminal is a file of tty.c's own, and both names reach it: the
 * caller's terminal and the machine's console are the one console
 */
struct dev_entry {
    const char *str_name;
    uint32_t u_mode;
    const struct file_operations *p_ops;
};

static const struct dev_entry s_arr_devs[] = {
    { "null",    0666, &null_ops },
    { "zero",    0666, &zero_ops },
    { "full",    0666, &full_ops },
    { "random",  0666, &random_ops },
    { "urandom", 0666, &random_ops },
    { "tty",     0666, NULL },
    { "console", 0600, NULL },
};

#define DEV_COUNT (sizeof s_arr_devs / sizeof s_arr_devs[0])

size_t dev_count(void)
{
    return DEV_COUNT;
}

const char *dev_name(size_t sz_index)
{
    return (sz_index < DEV_COUNT) ? s_arr_devs[sz_index].str_name : NULL;
}

uint32_t dev_mode(size_t sz_index)
{
    return (sz_index < DEV_COUNT) ? s_arr_devs[sz_index].u_mode : 0;
}

bool dev_find(const char *str_name, size_t *p_index)
{
    for (size_t sz_at = 0; sz_at < DEV_COUNT; sz_at++) {
        if (strcmp(s_arr_devs[sz_at].str_name, str_name) == 0) {
            *p_index = sz_at;
            return true;
        }
    }
    return false;
}

struct file *dev_open(size_t sz_index, uint32_t u_mode)
{
    if (sz_index >= DEV_COUNT) {
        return NULL;
    }
    if (s_arr_devs[sz_index].p_ops == NULL) {
        return tty_console_file();
    }
    return file_alloc(s_arr_devs[sz_index].p_ops, u_mode, FILE_TYPE_CHAR);
}

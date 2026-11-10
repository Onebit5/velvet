// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/proc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /proc: files the kernel writes at the moment they are read.
 */

#include <velvet/proc.h>
#include <asm/cpu.h>
#include <velvet/file.h>
#include <velvet/kmalloc.h>
#include <velvet/kprintf.h>
#include <velvet/pit.h>
#include <velvet/sched.h>
#include <velvet/spinlock.h>
#include <velvet/string.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/fcntl.h>
#include <generated/version.h>

/*
 * a file here is a name and a function. the function writes the file's
 * text with kprintf, the same way the kernel writes anything for a
 * person, and a capture turns that into bytes for whoever reads it. so a
 * subsystem that can describe itself on the screen can describe itself
 * to a program, with the same words, by registering one name.
 *
 * nothing is stored but by an open file. a read from the start writes
 * the file again and the open keeps that text for the reads after it,
 * which is why a file here has no size until it is read, why two opens a
 * second apart may disagree, and why one open read in pieces never
 * does: the pieces are one answer, not the ends of several
 */
struct proc_file {
    char arr_name[PROC_NAME_MAX];
    uint32_t u_mode;
    void (*p_show)(void);
};

static struct proc_file s_arr_files[PROC_FILES_MAX];
static size_t s_sz_count;

bool proc_register(const char *str_name, uint32_t u_mode,
                   void (*p_show)(void))
{
    size_t sz_index;

    if (s_sz_count >= PROC_FILES_MAX || strlen(str_name) >= PROC_NAME_MAX
        || proc_find(str_name, &sz_index)) {
        return false;
    }
    strcpy(s_arr_files[s_sz_count].arr_name, str_name);
    s_arr_files[s_sz_count].u_mode = u_mode;
    s_arr_files[s_sz_count].p_show = p_show;
    s_sz_count++;
    return true;
}

bool proc_find(const char *str_name, size_t *p_index)
{
    for (size_t sz_at = 0; sz_at < s_sz_count; sz_at++) {
        if (strcmp(s_arr_files[sz_at].arr_name, str_name) == 0) {
            *p_index = sz_at;
            return true;
        }
    }
    return false;
}

const char *proc_name(size_t sz_index)
{
    return (sz_index < s_sz_count) ? s_arr_files[sz_index].arr_name : NULL;
}

uint32_t proc_mode(size_t sz_index)
{
    return (sz_index < s_sz_count) ? s_arr_files[sz_index].u_mode : 0;
}

int64_t proc_render(size_t sz_index, char *str_buf, size_t sz_size)
{
    if (sz_index >= s_sz_count) {
        return -1;
    }
    struct kcapture cap = { str_buf, sz_size, 0, false };

    sched_capture(&cap);
    s_arr_files[sz_index].p_show();
    sched_capture(NULL);
    return (int64_t)cap.sz_len;
}

/*
 * an open file of /proc: which file, and the text it was last written
 * with, NULL before the first read and for a file that said nothing.
 * the text is swapped under the lock and read under it, so a thread
 * reading from the start while another reads further on hands the
 * second either the old text or the new, never one being freed
 */
struct proc_open {
    struct spinlock lock;
    size_t sz_index;
    bool b_written;
    char *p_text;
    size_t sz_len;
};

/*
 * the file's text as it is now, in memory of its own length, into
 * *pp_text: false with no memory to write it in
 */
static bool written(size_t sz_index, char **pp_text, size_t *p_len)
{
    char *str_whole = kmalloc(PROC_TEXT_MAX);
    if (str_whole == NULL) {
        return false;
    }
    int64_t i_len = proc_render(sz_index, str_whole, PROC_TEXT_MAX);
    char *p_text = (i_len > 0) ? kmalloc((size_t)i_len) : NULL;
    bool b_ok = i_len == 0 || p_text != NULL;
    if (p_text != NULL) {
        memcpy(p_text, str_whole, (size_t)i_len);
    }
    kfree(str_whole);
    *pp_text = p_text;
    *p_len = (i_len > 0) ? (size_t)i_len : 0;
    return b_ok;
}

static int64_t open_read(struct file *p_file, void *p_buf, size_t sz_len)
{
    struct proc_open *p_open = p_file->p_private;

    if (p_file->u_pos == 0 || !p_open->b_written) {
        char *p_text;
        size_t sz_text;
        if (!written(p_open->sz_index, &p_text, &sz_text)) {
            return -ENOMEM;
        }
        uint64_t u_flags = spin_lock_irq(&p_open->lock);
        char *p_old = p_open->p_text;
        p_open->p_text = p_text;
        p_open->sz_len = sz_text;
        p_open->b_written = true;
        spin_unlock_irq(&p_open->lock, u_flags);
        kfree(p_old);
    }
    uint64_t u_flags = spin_lock_irq(&p_open->lock);
    uint64_t u_left = (p_file->u_pos < p_open->sz_len)
                      ? p_open->sz_len - p_file->u_pos : 0;
    size_t sz_got = (sz_len < u_left) ? sz_len : (size_t)u_left;
    if (sz_got > 0) {
        memcpy(p_buf, p_open->p_text + p_file->u_pos, sz_got);
    }
    spin_unlock_irq(&p_open->lock, u_flags);
    p_file->u_pos += sz_got;
    return (int64_t)sz_got;
}

/*
 * from the start or from where it is. the end is wherever the next
 * read from the start says, so there is none to seek from
 */
static int64_t open_seek(struct file *p_file, int64_t i_offset, int i_whence)
{
    int64_t i_from = (i_whence == SEEK_SET) ? 0
                   : (i_whence == SEEK_CUR) ? (int64_t)p_file->u_pos
                   : -1;
    if (i_from < 0 || i_from + i_offset < 0) {
        return -EINVAL;
    }
    p_file->u_pos = (uint64_t)(i_from + i_offset);
    return (int64_t)p_file->u_pos;
}

static void open_release(struct file *p_file)
{
    struct proc_open *p_open = p_file->p_private;

    kfree(p_open->p_text);
    kfree(p_open);
}

static const struct file_operations s_open_ops = {
    .read = open_read,
    .seek = open_seek,
    .release = open_release,
};

struct file *proc_open(size_t sz_index, uint32_t u_mode)
{
    if (sz_index >= s_sz_count) {
        return NULL;
    }
    struct proc_open *p_open = kmalloc(sizeof *p_open);
    struct file *p_file = (p_open != NULL)
                          ? file_alloc(&s_open_ops, u_mode,
                                       FILE_TYPE_REGULAR)
                          : NULL;
    if (p_file == NULL) {
        kfree(p_open);
        return NULL;
    }
    memset(p_open, 0, sizeof *p_open);
    spin_init_unlisted(&p_open->lock, "proc file", LOCK_RANK_DEVICE);
    p_open->sz_index = sz_index;
    p_file->p_private = p_open;
    return p_file;
}

/*
 * what is running, the compiler that made it and when: version, then
 * the compiler in brackets, then the date. a program wanting one of
 * them splits the line rather than opening three files
 */
static void show_version(void)
{
    kprintf("velvet version %s (gcc %s) %s %s\n", VELVET_VERSION,
            __VERSION__, __DATE__, __TIME__);
}

/* how long it has been up, in seconds, to the hundredth. */
static void show_uptime(void)
{
    uint64_t u_ms = pit_uptime_ms();

    kprintf("%lu.%02lu\n", u_ms / 1000, (u_ms % 1000) / 10);
}

void proc_init(void)
{
    proc_register("version", PROC_PUBLIC, show_version);
    proc_register("uptime", PROC_PUBLIC, show_uptime);
    proc_register("cpuinfo", PROC_PUBLIC, cpu_show);
}

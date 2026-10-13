// SPDX-License-Identifier: GPL-2.0-only
/*
 * fs/proc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /proc: files the kernel writes at the moment they are read.
 */

#include <velvet/proc.h>
#include <asm/cpu.h>
#include <velvet/kmalloc.h>
#include <velvet/kprintf.h>
#include <velvet/pit.h>
#include <velvet/sched.h>
#include <velvet/string.h>
#include <generated/version.h>

/*
 * a file here is a name and a function. the function writes the file's
 * text with kprintf, the same way the kernel writes anything for a
 * person, and a capture turns that into bytes for whoever reads it. so a
 * subsystem that can describe itself on the screen can describe itself
 * to a program, with the same words, by registering one name.
 *
 * nothing is stored. every read writes the file again, which is why a
 * file here has no size until it is read and two reads a second apart
 * may disagree: they are two answers to the same question
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

int64_t proc_read(size_t sz_index, uint64_t u_offset, void *p_buf,
                  uint64_t u_len)
{
    char *str_text = kmalloc(PROC_TEXT_MAX);
    int64_t i_got = -1;

    if (str_text == NULL) {
        return -1;
    }
    int64_t i_len = proc_render(sz_index, str_text, PROC_TEXT_MAX);
    if (i_len >= 0) {
        uint64_t u_left = (u_offset < (uint64_t)i_len)
                        ? (uint64_t)i_len - u_offset : 0;
        i_got = (int64_t)(u_len < u_left ? u_len : u_left);
        memcpy(p_buf, str_text + u_offset, (size_t)i_got);
    }
    kfree(str_text);
    return i_got;
}

/* what is running. the same words the boot banner starts with */
static void show_version(void)
{
    kprintf("velvet version %s\n", VELVET_VERSION);
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

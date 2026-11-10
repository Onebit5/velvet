// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_proc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for /proc: registering a file, and reading what it says.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* the lock complains through this when something is wrong with it */
void panic(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(1);
}

#include <velvet/file.h>
#include <velvet/kprintf.h>
#include <velvet/proc.h>
#include <velvet/slab.h>
#include <uapi/velvet/errno.h>
#include <uapi/velvet/fcntl.h>

/* what kprintf.c and proc.c lean on, done the plainest way there is */
void serial_putchar(char c_ch)
{
    (void)c_ch;
}
bool console_ready(void)
{
    return false;
}
void console_putchar(char c_ch)
{
    (void)c_ch;
}
void *kmalloc(size_t sz_size)
{
    return malloc(sz_size);
}
void kfree(void *p_mem)
{
    free(p_mem);
}
uint64_t pit_uptime_ms(void)
{
    return 61234;
}

/* the slab an open file comes from, as plain malloc */
void slab_cache_init(struct slab_cache *cache, const char *name,
                     size_t sz_obj)
{
    (void)name;
    cache->obj_size = sz_obj;
}
void *slab_alloc(struct slab_cache *cache)
{
    return malloc(cache->obj_size);
}
void slab_free(void *object)
{
    free(object);
}

/* one thread, so the capture is a single pointer kprintf is told about. */
static struct kcapture *s_p_capture;

static struct kcapture *capturing(void)
{
    return s_p_capture;
}

void sched_capture(struct kcapture *p_cap)
{
    s_p_capture = p_cap;
}

static int s_i_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_i_failures++; } \
} while (0)

static void show_greeting(void)
{
    kprintf("hello from %s, %d lines\n", "the kernel", 1);
}

static void show_nothing(void)
{
}

/* a file that says something different every time it is asked */
static int s_i_asked;

static void show_count(void)
{
    s_i_asked++;
    kprintf("asked %d times, and this is the line after\n", s_i_asked);
}

/* the whole of an open file from where it is, `sz_step` bytes a read */
static size_t read_rest(struct file *p_file, char *str_out, size_t sz_step)
{
    size_t sz_at = 0;
    for (;;) {
        int64_t i_got = p_file->p_ops->read(p_file, str_out + sz_at,
                                            sz_step);
        if (i_got <= 0) {
            break;
        }
        sz_at += (size_t)i_got;
    }
    str_out[sz_at] = '\0';
    return sz_at;
}

int main(void)
{
    char arr_text[256];
    size_t sz_index = 99;

    kprintf_capture_source(capturing);
    proc_init();

    CHECK(proc_find("version", &sz_index) && sz_index == 0,
          "version is the first file /proc has");
    CHECK(proc_find("uptime", &sz_index) && sz_index == 1,
          "and uptime the second");
    CHECK(proc_register("greeting", PROC_PUBLIC, show_greeting),
          "a file registers");
    CHECK(!proc_register("greeting", PROC_PUBLIC, show_nothing),
          "but not twice under one name");
    CHECK(!proc_register("a name far too long for any proc file",
                         PROC_PUBLIC, show_nothing),
          "nor under a name that does not fit");
    CHECK(proc_register("empty", PROC_PRIVATE, show_nothing),
          "an empty one is fine, and so is root's own");
    CHECK(!proc_find("nothing", &sz_index), "a name nobody registered");

    CHECK(proc_find("cpuinfo", &sz_index) && sz_index == 2,
          "and cpuinfo, which the architecture writes, the third");
    CHECK(strcmp(proc_name(3), "greeting") == 0, "listed by its name");
    CHECK(proc_name(5) == NULL, "and the listing ends");
    CHECK(proc_mode(3) == 0444 && proc_mode(4) == 0400,
          "each keeps the mode it was registered with");
    CHECK(proc_mode(9) == 0, "and one that does not exist has none");

    int64_t i_len = proc_render(3, arr_text, sizeof arr_text);
    CHECK(i_len == 31 && memcmp(arr_text, "hello from the kernel, 1 lines\n",
                                31) == 0, "its text is what it printed");
    CHECK(s_p_capture == NULL, "and printing goes back to normal after");
    CHECK(proc_render(4, arr_text, sizeof arr_text) == 0,
          "a file that says nothing is empty");
    CHECK(proc_render(9, arr_text, sizeof arr_text) == -1,
          "a file that does not exist cannot be written");

    i_len = proc_render(1, arr_text, sizeof arr_text);
    CHECK(i_len == 6 && memcmp(arr_text, "61.23\n", 6) == 0,
          "uptime is seconds to the hundredth");
    i_len = proc_render(0, arr_text, sizeof arr_text);
    CHECK(i_len > 15 && memcmp(arr_text, "velvet version ", 15) == 0,
          "version says which kernel");
    arr_text[i_len] = '\0';
    CHECK(strstr(arr_text, " (gcc ") != NULL && arr_text[i_len - 1] == '\n',
          "and which compiler made it, on one line");

    /* read in pieces, it is the same file */
    char arr_whole[128];
    struct file *p_file = proc_open(3, FILE_READ);
    CHECK(p_file != NULL && p_file->u_refs == 1 && p_file->p_ops->seek
          && p_file->p_ops->write == NULL, "a file opens, for reading");
    CHECK(read_rest(p_file, arr_whole, 5) == 31
          && strcmp(arr_whole, "hello from the kernel, 1 lines\n") == 0,
          "five bytes at a time adds up");
    CHECK(p_file->p_ops->read(p_file, arr_whole, 5) == 0,
          "and the end is a read of nothing");
    file_put(p_file);
    CHECK(proc_open(9, FILE_READ) == NULL, "no such file, no open");

    /* one open is one moment, and a read from the start is the next */
    CHECK(proc_register("count", PROC_PUBLIC, show_count),
          "a file that changes registers");
    p_file = proc_open(5, FILE_READ);
    CHECK(read_rest(p_file, arr_whole, 7) == 42
          && strcmp(arr_whole, "asked 1 times, and this is the line after\n")
             == 0 && s_i_asked == 1,
          "read seven bytes at a time, it was asked once");
    CHECK(p_file->p_ops->seek(p_file, 6, SEEK_SET) == 6
          && read_rest(p_file, arr_whole, 64) == 36
          && strncmp(arr_whole, "1 times", 7) == 0 && s_i_asked == 1,
          "a read from further on reads what was written then");
    CHECK(p_file->p_ops->seek(p_file, -3, SEEK_CUR) == 39
          && read_rest(p_file, arr_whole, 64) == 3
          && strcmp(arr_whole, "er\n") == 0,
          "and from where it is");
    CHECK(p_file->p_ops->seek(p_file, 0, SEEK_END) == -EINVAL
          && p_file->p_ops->seek(p_file, -50, SEEK_CUR) == -EINVAL
          && p_file->p_ops->seek(p_file, -1, SEEK_SET) == -EINVAL,
          "but not from the end, nor to before the start");
    CHECK(p_file->p_ops->seek(p_file, 0, SEEK_SET) == 0
          && read_rest(p_file, arr_whole, 64) == 42
          && strncmp(arr_whole, "asked 2 times", 13) == 0 && s_i_asked == 2,
          "a read from the start asks again");
    struct file *p_other = proc_open(5, FILE_READ);
    CHECK(p_other->p_ops->seek(p_other, 10, SEEK_SET) == 10
          && read_rest(p_other, arr_whole, 64) == 32 && s_i_asked == 3,
          "an open first read further on is asked then");
    CHECK(p_file->p_ops->seek(p_file, 4, SEEK_SET) == 4
          && read_rest(p_file, arr_whole, 64) == 38
          && strncmp(arr_whole, "d 2 ", 4) == 0 && s_i_asked == 3,
          "and leaves the other open's moment alone");
    file_put(p_other);
    file_put(p_file);

    p_file = proc_open(4, FILE_READ);
    CHECK(read_rest(p_file, arr_whole, 64) == 0,
          "a file that says nothing reads as nothing");
    file_put(p_file);

    if (s_i_failures == 0) {
        printf("all good\n");
    }
    return s_i_failures;
}

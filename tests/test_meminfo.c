// SPDX-License-Identifier: GPL-2.0-only
/*
 * tests/test_meminfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * host-side test for what memory says about itself in /proc: each file's
 * layout, to the space, over counts made up for the purpose.
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

#include <velvet/kprintf.h>
#include <velvet/meminfo.h>
#include <velvet/pmm.h>
#include <velvet/pressure.h>
#include <velvet/proc.h>
#include <velvet/ramfs.h>
#include <velvet/slab.h>
#include <velvet/thread.h>
#include <velvet/vmm.h>

/* what kprintf.c leans on, done the plainest way there is */
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

/*
 * a machine of 512 MiB with 300 free, eight of it held back, three in
 * files, two object caches and twelve threads
 */
uint64_t pmm_total_bytes(void)
{
    return 512ull << 20;
}
uint64_t pmm_free_bytes(void)
{
    return 300ull << 20;
}
uint64_t pressure_available(uint64_t u_free_pages)
{
    return u_free_pages - 2048;
}
uint64_t ramfs_bytes(void)
{
    return 3ull << 20;
}
uint64_t thread_stack_bytes(void)
{
    return 12 * 5 * PAGE_SIZE;
}
uint64_t pmm_blocks_at(unsigned u_order)
{
    return (u_order == 10) ? 70 : u_order * 3;
}

static struct slab_cache s_threads = {
    .name = "thread", .obj_size = 1024, .per_slab = 3,
    .pages = 4, .in_use = 10,
};
static struct slab_cache s_small = {
    .name = "kmalloc-64", .obj_size = 64, .per_slab = 63,
    .pages = 3, .in_use = 150, .next = &s_threads,
};
struct slab_cache *slab_first_cache(void)
{
    return &s_small;
}

/* the files, as they are registered: a name, who reads it, what says it */
struct registered {
    const char *str_name;
    uint32_t u_mode;
    void (*fn_show)(void);
};
static struct registered s_arr_files[8];
static size_t s_sz_files;

bool proc_register(const char *str_name, uint32_t u_mode, void (*fn)(void))
{
    s_arr_files[s_sz_files++] = (struct registered){ str_name, u_mode, fn };
    return true;
}
void vmm_show(void)
{
    kprintf("the page tables\n");
}

static struct kcapture *s_p_capture;

static struct kcapture *capturing(void)
{
    return s_p_capture;
}

static int s_i_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); s_i_failures++; } \
} while (0)

/* what the file called str_name says, or "" for no such file */
static const char *shown(const char *str_name, uint32_t *p_mode)
{
    static char s_arr_text[4096];
    struct kcapture cap = { s_arr_text, sizeof s_arr_text - 1, 0, false };
    for (size_t sz_at = 0; sz_at < s_sz_files; sz_at++) {
        if (strcmp(s_arr_files[sz_at].str_name, str_name) == 0) {
            s_p_capture = &cap;
            s_arr_files[sz_at].fn_show();
            s_p_capture = NULL;
            s_arr_text[cap.sz_len] = '\0';
            *p_mode = s_arr_files[sz_at].u_mode;
            return s_arr_text;
        }
    }
    return "";
}

int main(void)
{
    uint32_t u_mode = 0;

    kprintf_capture_source(capturing);
    mm_proc_init();

    CHECK(strcmp(shown("meminfo", &u_mode),
                 "MemTotal:         524288 kB\n"
                 "MemFree:          307200 kB\n"
                 "MemAvailable:     299008 kB\n"
                 "Buffers:               0 kB\n"
                 "Cached:             3072 kB\n"
                 "SwapCached:            0 kB\n"
                 "SwapTotal:             0 kB\n"
                 "SwapFree:              0 kB\n"
                 "Slab:                 28 kB\n"
                 "SReclaimable:          0 kB\n"
                 "SUnreclaim:           28 kB\n"
                 "KernelStack:         240 kB\n") == 0
          && u_mode == PROC_PUBLIC,
          "meminfo: a name in sixteen and kilobytes in eight, anyone's");

    CHECK(strcmp(shown("slabinfo", &u_mode),
                 "slabinfo - version: 2.1\n"
                 "# name            <active_objs> <num_objs> <objsize>"
                 " <objperslab> <pagesperslab> : tunables <limit>"
                 " <batchcount> <sharedfactor> : slabdata <active_slabs>"
                 " <num_slabs> <sharedavail>\n"
                 "kmalloc-64           150    189     64   63    1"
                 " : tunables    0    0    0 : slabdata      3      3      0\n"
                 "thread                10     12   1024    3    1"
                 " : tunables    0    0    0 : slabdata      4      4      0\n")
          == 0,
          "slabinfo: version 2.1, a cache a line, the room each has");
    CHECK(u_mode == PROC_PRIVATE, "and root's alone");

    CHECK(strcmp(shown("buddyinfo", &u_mode),
                 "Node 0, zone   Normal "
                 "     0      3      6      9     12     15     18     21"
                 "     24     27     70 \n") == 0
          && u_mode == PROC_PUBLIC,
          "buddyinfo: one zone, the free blocks of every size, anyone's");

    CHECK(strcmp(shown("vmm", &u_mode), "the page tables\n") == 0
          && u_mode == PROC_PRIVATE,
          "and the kernel's page tables stay root's");

    if (s_i_failures == 0) {
        printf("all good\n");
    }
    return s_i_failures;
}

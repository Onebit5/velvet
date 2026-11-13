// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/mm/meminfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what memory says about itself, read from /proc the way free and top
 * read it, and held against what sysinfo and the program itself know.
 */

#include "../kselftest.h"

#include <ctype.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>

#define TEXT_MAX 16384
#define ORDERS   11             /* free blocks of one frame up to 1024 */

static char s_arr_text[TEXT_MAX];

static int64_t slurp(const char *str_path)
{
    int64_t i_len = ksft_read_file(str_path, s_arr_text, TEXT_MAX - 1);
    s_arr_text[(i_len > 0) ? i_len : 0] = '\0';
    return i_len;
}

/* the kilobytes on meminfo's line for str_name, or -1 for no such line */
static long long kb(const char *str_name)
{
    size_t sz_name = strlen(str_name);
    for (const char *p_line = s_arr_text; *p_line != '\0';) {
        if (strncmp(p_line, str_name, sz_name) == 0
            && p_line[sz_name] == ':') {
            return strtoll(p_line + sz_name + 1, NULL, 10);
        }
        const char *p_end = strchr(p_line, '\n');
        p_line = (p_end != NULL) ? p_end + 1 : "";
    }
    return -1;
}

/* every line a name, a colon, spaces and a count, in kilobytes or not */
static bool lines_are_counts(void)
{
    int i_lines = 0;
    for (const char *p_at = s_arr_text; *p_at != '\0'; i_lines++) {
        const char *p_name = p_at;
        while (isalnum((unsigned char)*p_at) || *p_at == '_' || *p_at == '('
               || *p_at == ')') {
            p_at++;
        }
        if (p_at == p_name || *p_at++ != ':' || *p_at != ' ') {
            return false;
        }
        while (*p_at == ' ') {
            p_at++;
        }
        if (!isdigit((unsigned char)*p_at)) {
            return false;
        }
        while (isdigit((unsigned char)*p_at)) {
            p_at++;
        }
        p_at += (strncmp(p_at, " kB", 3) == 0) ? 3 : 0;
        if (*p_at++ != '\n') {
            return false;
        }
    }
    return i_lines > 0;
}

static void check_meminfo(void)
{
    static const char *const s_arr_read[] = {
        "MemTotal", "MemFree", "MemAvailable", "Buffers", "Cached",
        "SwapTotal", "SwapFree", "Slab", "KernelStack",
    };
    bool b_shape = slurp("/proc/meminfo") > 0 && lines_are_counts();
    ksft_test_result(b_shape, "meminfo is a count a line, by name\n");

    bool b_all = true;
    for (size_t sz_at = 0; sz_at < sizeof s_arr_read / sizeof *s_arr_read;
         sz_at++) {
        b_all = b_all && kb(s_arr_read[sz_at]) >= 0;
    }
    long long i_total = kb("MemTotal");
    ksft_test_result(b_all && i_total > 0 && kb("MemFree") <= i_total
                     && kb("MemAvailable") <= i_total
                     && kb("SwapFree") <= kb("SwapTotal"),
                     "with what free and top read, none more than there"
                     " is\n");

    struct sysinfo info;
    bool b_info = sysinfo(&info) == 0;
    slurp("/proc/meminfo");
    long long i_free = (long long)(info.freeram * info.mem_unit / 1024);
    ksft_test_result(b_info
                     && (long long)(info.totalram * info.mem_unit / 1024)
                        == kb("MemTotal")
                     && llabs(i_free - kb("MemFree")) <= kb("MemTotal") / 16,
                     "sysinfo has the same total, and about as much free\n");

    struct stat st;
    bool b_self = stat("/proc/self/exe", &st) == 0;
    ksft_test_result(b_self && kb("Cached") >= (long long)st.st_size / 1024
                     && kb("Slab") > 0 && kb("KernelStack") >= 8,
                     "the files in memory, this program's among them, the"
                     " object caches and a stack for each thread\n");
}

/*
 * the free blocks buddyinfo counts, in kilobytes, every zone's: -1 when a
 * line is not a node, a zone and a count for every size
 */
static long long buddy_kb(void)
{
    long long i_kb = 0;
    int i_lines = 0;
    for (const char *p_at = s_arr_text; *p_at != '\0'; i_lines++) {
        int i_node;
        char arr_zone[32];
        int i_used = 0;
        if (sscanf(p_at, "Node %d, zone %31s%n", &i_node, arr_zone,
                   &i_used) != 2) {
            return -1;
        }
        p_at += i_used;
        for (int i_order = 0; i_order < ORDERS; i_order++) {
            char *p_end;
            long long i_blocks = strtoll(p_at, &p_end, 10);
            if (p_end == p_at || i_blocks < 0) {
                return -1;
            }
            i_kb += (i_blocks << i_order) * 4;
            p_at = p_end;
        }
        while (*p_at == ' ') {
            p_at++;
        }
        if (*p_at++ != '\n') {
            return -1;
        }
    }
    return (i_lines > 0) ? i_kb : -1;
}

static void check_buddyinfo(void)
{
    slurp("/proc/meminfo");
    long long i_total = kb("MemTotal");
    long long i_free = kb("MemFree");
    slurp("/proc/buddyinfo");
    long long i_blocks = buddy_kb();
    ksft_test_result(i_blocks >= 0,
                     "buddyinfo names a zone a line and counts its free"
                     " blocks of every size\n");
    ksft_test_result(i_blocks >= 0 && llabs(i_blocks - i_free) <= i_total / 16,
                     "and they add up to what meminfo says is free\n");
}

/*
 * slabinfo is root's. whoever opens it reads version 2.1; whoever is
 * refused is refused for want of permission, and anybody not root is
 * refused. root in a namespace of its own may not be root to the file
 */
static void check_slabinfo(void)
{
    struct stat st;
    bool b_mode = stat("/proc/slabinfo", &st) == 0
                  && (st.st_mode & 07777) == 0400;
    errno = 0;
    int64_t i_len = slurp("/proc/slabinfo");
    int i_errno = errno;
    bool b_read = i_len > 0
                  && strncmp(s_arr_text, "slabinfo - version: 2.1\n"
                                         "# name ", 31) == 0;
    bool b_refused = i_len < 0 && i_errno == EACCES;
    ksft_test_result(b_mode && (b_read || b_refused)
                     && (geteuid() == 0 || b_refused),
                     "slabinfo is version 2.1, and root's\n");
}

int main(void)
{
    ksft_print_header();
    ksft_set_plan(7);
    check_meminfo();
    check_buddyinfo();
    check_slabinfo();
    ksft_finished();
}

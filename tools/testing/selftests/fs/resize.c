// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/resize.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * resize, asked for by a program that is not root.
 */

#include "../kselftest.h"

#include <sys/ioctl.h>
#include <velvet/ext4.h>

#define TEXT_MAX 4096
#define SIZE_LEN 64

static char s_arr_text[TEXT_MAX];

/* where `str_needle` starts in `str_text`, from `i_from` on, or -1 */
static int64_t find(const char *str_text, int64_t i_len, int64_t i_from,
                    const char *str_needle)
{
    size_t sz_need = strlen(str_needle);

    for (int64_t i_at = i_from; i_at + (int64_t)sz_need <= i_len; i_at++) {
        if (memcmp(str_text + i_at, str_needle, sz_need) == 0) {
            return i_at;
        }
    }
    return -1;
}

/*
 * how big the filesystem mounted at `str_at` is, as /proc/disk says it
 * under that mount: the "of N MiB" end of its "used" line, which is the
 * size and not how much of it is in use. false when /proc/disk has no
 * such mount, or says nothing about its size
 */
static bool size_at(const char *str_at, char *str_out)
{
    char arr_mount[SIZE_LEN];
    int64_t i_len = ksft_read_file("/proc/disk", s_arr_text, TEXT_MAX);
    size_t sz_out = 0;

    snprintf(arr_mount, sizeof arr_mount, "mounted at %s\n", str_at);
    int64_t i_from = find(s_arr_text, i_len, 0, arr_mount);
    if (i_from < 0) {
        return false;
    }
    int64_t i_end = find(s_arr_text, i_len, i_from, "\n\n");
    if (i_end < 0) {
        i_end = i_len;
    }
    int64_t i_used = find(s_arr_text, i_end, i_from, "\nused ");
    int64_t i_of = i_used < 0 ? -1 : find(s_arr_text, i_end, i_used, " of ");
    if (i_of < 0) {
        return false;
    }
    for (int64_t i_at = i_of; i_at < i_end && s_arr_text[i_at] != '\n'
         && sz_out + 1 < SIZE_LEN; i_at++) {
        str_out[sz_out++] = s_arr_text[i_at];
    }
    str_out[sz_out] = '\0';
    return true;
}

int main(void)
{
    char arr_was[SIZE_LEN];
    char arr_now[SIZE_LEN];
    uint64_t u_blocks = 0;

    ksft_print_header();
    ksft_set_plan(4);

    bool b_was = size_at("/work", arr_was);
    int i_work = open("/work", O_RDONLY | O_DIRECTORY);
    ksft_test_result(ioctl(i_work, EXT4_IOC_RESIZE_FS, &u_blocks) == -1
                     && errno == EPERM,
                     "a guest may not resize a filesystem\n");
    ksft_test_result(b_was && size_at("/work", arr_now)
                     && ksft_streq(arr_was, arr_now),
                     "and it is the size it was\n");
    int i_tests = open("/proc", O_RDONLY | O_DIRECTORY);
    ksft_test_result(ioctl(i_tests, EXT4_IOC_RESIZE_FS, &u_blocks) == -1
                     && errno == ENOTTY,
                     "one that is not ext4 does not know the request\n");
    ksft_test_result(ioctl(i_work, EXT4_IOC_RESIZE_FS,
                           (void *)0xffff800000000000ull) == -1
                     && errno == EFAULT,
                     "and the count comes from nowhere but the caller's "
                     "memory\n");
    ksft_finished();
}

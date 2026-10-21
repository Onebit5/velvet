// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/resize.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * resize, asked for by a program that is not root.
 */

#include "../kselftest.h"

#define TEXT_MAX 4096
#define SIZE_LEN 64

static struct user_resize s_resize;
static char s_arr_text[TEXT_MAX];

static int64_t resize(const char *str_at, uint64_t u_blocks,
                      struct user_resize *p_out)
{
    return ksft_syscall5(SYS_RESIZE, (int64_t)str_at,
                         (int64_t)ksft_strlen(str_at), (int64_t)u_blocks,
                         (int64_t)p_out, 0);
}

/* where `str_needle` starts in `str_text`, from `i_from` on, or -1 */
static int64_t find(const char *str_text, int64_t i_len, int64_t i_from,
                    const char *str_needle)
{
    size_t sz_need = ksft_strlen(str_needle);

    for (int64_t i_at = i_from; i_at + (int64_t)sz_need <= i_len; i_at++) {
        if (ksft_memeq(str_text + i_at, str_needle, sz_need)) {
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
    char arr_mount[SIZE_LEN] = "mounted at ";
    size_t sz_at = ksft_strlen(arr_mount);
    int64_t i_len = ksft_read_file("/proc/disk", s_arr_text, TEXT_MAX);
    int64_t i_from;
    int64_t i_end;
    int64_t i_used;
    int64_t i_of;
    size_t sz_out = 0;

    for (size_t sz_i = 0; str_at[sz_i] != '\0' && sz_at + 2 < SIZE_LEN;
         sz_i++) {
        arr_mount[sz_at++] = str_at[sz_i];
    }
    arr_mount[sz_at++] = '\n';
    arr_mount[sz_at] = '\0';

    i_from = find(s_arr_text, i_len, 0, arr_mount);
    if (i_from < 0) {
        return false;
    }
    i_end = find(s_arr_text, i_len, i_from, "\n\n");
    if (i_end < 0) {
        i_end = i_len;
    }
    i_used = find(s_arr_text, i_end, i_from, "\nused ");
    i_of = i_used < 0 ? -1 : find(s_arr_text, i_end, i_used, " of ");
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

int main(int i_argc, char **pp_argv)
{
    char arr_was[SIZE_LEN];
    char arr_now[SIZE_LEN];
    bool b_was;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(4);

    b_was = size_at("/work", arr_was);
    ksft_test_result(resize("/work", 0, &s_resize) == -1
                     && s_resize.arr_why[0] != '\0',
                     "a guest may not resize, and is told why\n");
    ksft_test_result(b_was && size_at("/work", arr_now)
                     && ksft_streq(arr_was, arr_now),
                     "and the filesystem is the size it was\n");
    ksft_test_result(resize("/selftests", 0, &s_resize) == -1,
                     "a path nothing is mounted at is refused\n");
    ksft_test_result(resize("/work", 0,
                            (struct user_resize *)0xffff800000000000ull)
                     == -1,
                     "and the answer goes nowhere but the caller's memory\n");
    ksft_finished();
}

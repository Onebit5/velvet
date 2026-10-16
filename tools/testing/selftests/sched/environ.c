// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/environ.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /proc/environ, the reader's own environment, for a program that has
 * to know which variables there are rather than ask for one by name.
 */

#include "../kselftest.h"

#define TEXT_MAX 2048

static char s_arr_text[TEXT_MAX];

static void set_var(const char *str_name, const char *str_value)
{
    ksft_syscall5(SYS_SETENV, (int64_t)str_name,
                  (int64_t)ksft_strlen(str_name), (int64_t)str_value,
                  (int64_t)ksft_strlen(str_value), 0);
}

/* whether `str_entry`, with its NUL, is one of the entries */
static bool listed(int64_t i_len, const char *str_entry)
{
    size_t sz_need = ksft_strlen(str_entry) + 1;
    int64_t i_at = 0;

    while (i_at < i_len) {
        if (ksft_memeq(s_arr_text + i_at, str_entry, sz_need)) {
            return true;
        }
        while (i_at < i_len && s_arr_text[i_at] != '\0') {
            i_at++;
        }
        i_at++;
    }
    return false;
}

int main(int i_argc, char **pp_argv)
{
    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(3);

    set_var("SELFTEST_ONE", "first");
    set_var("SELFTEST_TWO", "second");
    int64_t i_len = ksft_read_file("/proc/environ", s_arr_text, TEXT_MAX);

    ksft_test_result(listed(i_len, "SELFTEST_ONE=first"),
                     "/proc/environ lists a variable the reader set\n");
    ksft_test_result(listed(i_len, "SELFTEST_TWO=second"),
                     "and the next, each ended by a NUL\n");
    ksft_test_result(i_len > 0 && s_arr_text[i_len - 1] == '\0'
                     && (i_len < 2 || s_arr_text[i_len - 2] != '\0'),
                     "with no empty entry after the last\n");
    ksft_finished();
}

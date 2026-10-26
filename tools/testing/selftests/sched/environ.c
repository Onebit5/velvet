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

/* whether `str_entry`, with its NUL, is one of the entries */
static bool listed(int64_t i_len, const char *str_entry)
{
    size_t sz_need = strlen(str_entry) + 1;
    int64_t i_at = 0;

    while (i_at < i_len) {
        if (memcmp(s_arr_text + i_at, str_entry, sz_need) == 0) {
            return true;
        }
        while (i_at < i_len && s_arr_text[i_at] != '\0') {
            i_at++;
        }
        i_at++;
    }
    return false;
}

/*
 * the program again, given an environment of the test's choosing: what
 * it reads of its own in /proc/environ, as a bit for each check, is its
 * exit status
 */
static int as_child(void)
{
    int64_t i_len = ksft_read_file("/proc/environ", s_arr_text, TEXT_MAX);
    int i_bits = 0;

    i_bits |= listed(i_len, "SELFTEST_ONE=first") ? 1 : 0;
    i_bits |= listed(i_len, "SELFTEST_TWO=second") ? 2 : 0;
    i_bits |= (i_len > 0 && s_arr_text[i_len - 1] == '\0'
               && (i_len < 2 || s_arr_text[i_len - 2] != '\0')) ? 4 : 0;
    i_bits |= (getenv("SELFTEST_TWO") != NULL
               && ksft_streq(getenv("SELFTEST_TWO"), "second")) ? 8 : 0;
    return i_bits;
}

int main(int i_argc, char **pp_argv)
{
    int i_status = 0;

    if (i_argc == 2 && ksft_streq(pp_argv[1], "child")) {
        return as_child();
    }
    ksft_print_header();
    ksft_set_plan(4);

    pid_t i_pid = fork();
    if (i_pid == 0) {
        char *arr_argv[] = { pp_argv[0], "child", NULL };
        char *arr_envp[] = { "SELFTEST_ONE=first", "SELFTEST_TWO=second",
                             NULL };
        execve(pp_argv[0], arr_argv, arr_envp);
        _exit(255);
    }
    waitpid(i_pid, &i_status, 0);
    int i_bits = WIFEXITED(i_status) ? WEXITSTATUS(i_status) : 0;

    ksft_test_result(i_bits != 255 && (i_bits & 1),
                     "/proc/environ lists what exec was given\n");
    ksft_test_result(i_bits != 255 && (i_bits & 2),
                     "and the next, each ended by a NUL\n");
    ksft_test_result(i_bits != 255 && (i_bits & 4),
                     "with no empty entry after the last\n");
    ksft_test_result(i_bits != 255 && (i_bits & 8),
                     "and the libc finds the same where exec put it\n");
    ksft_finished();
}

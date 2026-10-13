// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/lib/syslog.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the kernel's log, read by a program the way dmesg reads it.
 */

#include "../kselftest.h"

#define LOG_MAX (64 * 1024)
#define SHORT   16

static char s_arr_log[LOG_MAX];

int main(int i_argc, char **pp_argv)
{
    char arr_short[SHORT];
    int64_t i_size;
    int64_t i_first;
    int64_t i_again;

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(6);

    i_size = klogctl(SYSLOG_SIZE_BUFFER, NULL, 0);
    ksft_test_result(i_size > 0 && i_size <= LOG_MAX,
                     "the log says how big a buffer all of it needs\n");
    if (i_size <= 0 || i_size > LOG_MAX) {
        i_size = LOG_MAX;
    }

    /* a kernel that booted far enough to run this has said something */
    i_first = klogctl(SYSLOG_READ_ALL, s_arr_log, i_size);
    ksft_test_result(i_first > 0 && i_first <= i_size,
                     "and reads into one that big\n");

    ksft_test_result(klogctl(SYSLOG_READ_ALL, arr_short, SHORT) == SHORT,
                     "a short buffer is filled to the brim\n");

    /* reading takes nothing out, the log only grows until it is full */
    i_again = klogctl(SYSLOG_READ_ALL, s_arr_log, i_size);
    ksft_test_result(i_again >= i_first,
                     "reading it leaves it all there for the next reader\n");

    ksft_test_result(klogctl(SYSLOG_READ_ALL,
                             (char *)0xffff800000000000ull, SHORT) == -1,
                     "the kernel's half is not somewhere to read it into\n");
    ksft_test_result(klogctl(0, s_arr_log, SHORT) == -1,
                     "and an action it does not know is refused\n");
    ksft_finished();
}

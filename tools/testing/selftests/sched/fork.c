// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/fork.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a forked child is its parent's copy: the environment it was holding,
 * what it had arranged to do with each signal, and memory of its own.
 */

#include "../kselftest.h"

#define NAME  "SELFTEST_FORK"
#define VALUE "carried"

/*
 * a page the parent has written, so that after a fork it is the one
 * page both hold, until one of them writes to it
 */
static char s_arr_page[4096] __attribute__((aligned(4096))) = "parent";

/* how many children, enough that every core of a small machine runs one */
#define CHILDREN 8

int main(void)
{
    int i_status = -1;

    ksft_print_header();
    ksft_set_plan(4);

    setenv(NAME, VALUE, 1);
    pid_t i_pid = fork();
    if (i_pid == 0) {
        const char *str_value = getenv(NAME);
        _exit(str_value != NULL && ksft_streq(str_value, VALUE) ? 0 : 1);
    }
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, 0),
                     "a forked child holds its parent's memory\n");

    signal(SIGTERM, SIG_IGN);
    i_pid = fork();
    if (i_pid == 0) {
        ksft_sleep_ms(300);
        _exit(0);
    }
    kill(i_pid, SIGTERM);
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, 0),
                     "and ignores what its parent chose to ignore\n");
    signal(SIGTERM, SIG_DFL);

    /*
     * a read is the kernel writing into the child's memory, and that is
     * a write to the shared page like any other: the child gets a copy
     * of its own and the parent's stays as it was, whichever core the
     * child ran on
     */
    bool b_kept = true;
    for (int i_at = 0; i_at < CHILDREN && b_kept; i_at++) {
        int arr_fds[2];
        if (pipe(arr_fds) != 0 || write(arr_fds[1], "child", 6) != 6) {
            b_kept = false;
            break;
        }
        s_arr_page[0] = 'p';
        i_pid = fork();
        if (i_pid == 0) {
            _exit(read(arr_fds[0], s_arr_page, 6) == 6
                  && ksft_streq(s_arr_page, "child") ? 0 : 1);
        }
        waitpid(i_pid, &i_status, 0);
        close(arr_fds[0]);
        close(arr_fds[1]);
        b_kept = ksft_exited(i_status, 0) && ksft_streq(s_arr_page, "parent");
    }
    ksft_test_result(b_kept,
                     "what the kernel writes into a child is the child's\n");

    /*
     * a vfork's parent waits until the child has run something or ended,
     * so what the child says, however late, comes first
     */
    char arr_order[2] = { 0, 0 };
    int arr_fds[2];
    pipe(arr_fds);
    i_pid = vfork();
    if (i_pid == 0) {
        ksft_sleep_ms(100);
        write(arr_fds[1], "c", 1);
        _exit(7);
    }
    write(arr_fds[1], "p", 1);
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(i_pid > 0 && ksft_exited(i_status, 7)
                     && read(arr_fds[0], arr_order, 2) == 2
                     && memcmp(arr_order, "cp", 2) == 0,
                     "vfork's parent goes on once its child is done\n");
    ksft_finished();
}

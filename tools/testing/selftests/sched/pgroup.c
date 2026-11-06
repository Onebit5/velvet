// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/pgroup.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * process groups: what a shell makes a job out of.
 */

#include "../kselftest.h"

#include <poll.h>

/* where a child that has been hung up on says so */
static int s_i_said = -1;

static void on_hup(int i_sig)
{
    (void)i_sig;
    write(s_i_said, "H", 1);
}

/*
 * a job, stopped, whose session's leader then ends: the job is orphaned,
 * nobody being left in the session to continue it. what it says on
 * arr_said as it goes, into str_out
 */
static void orphan_stopped(char *str_out)
{
    int arr_said[2];
    pipe(arr_said);
    pid_t i_lead = fork();
    if (i_lead == 0) {
        setsid();
        pid_t i_job = fork();
        if (i_job == 0) {
            setpgid(0, 0);
            s_i_said = arr_said[1];
            signal(SIGHUP, on_hup);
            raise(SIGSTOP);
            /* a terminal's stop, which an orphaned group does not take */
            raise(SIGTSTP);
            write(arr_said[1], "T", 1);
            _exit(0);
        }
        int i_status;
        waitpid(i_job, &i_status, WUNTRACED);
        _exit(0);
    }
    close(arr_said[1]);
    waitpid(i_lead, NULL, 0);
    struct pollfd fd = { .fd = arr_said[0], .events = POLLIN };
    size_t sz_len = 0;
    while (sz_len < 2 && poll(&fd, 1, 2000) == 1) {
        ssize_t i_got = read(arr_said[0], str_out + sz_len, 2 - sz_len);
        if (i_got <= 0) {
            break;
        }
        sz_len += (size_t)i_got;
    }
    str_out[sz_len] = '\0';
    close(arr_said[0]);
}

static pid_t sleeper(void)
{
    pid_t i_pid = fork();

    if (i_pid == 0) {
        for (;;) {
            ksft_sleep_ms(100);
        }
    }
    return i_pid;
}

int main(void)
{
    int i_one;
    int i_two;

    ksft_print_header();
    ksft_set_plan(5);

    pid_t i_left = sleeper();
    pid_t i_right = sleeper();
    ksft_test_result(setpgid(i_left, 0) == 0
                     && setpgid(i_right, i_left) == 0
                     && getpgid(i_right) == i_left,
                     "two children are put in one group\n");

    ksft_test_result(kill(-i_left, SIGTERM) == 0
                     && waitpid(i_left, &i_one, 0) == i_left
                     && waitpid(i_right, &i_two, 0) == i_right
                     && ksft_killed_by(i_one, SIGTERM)
                     && ksft_killed_by(i_two, SIGTERM),
                     "and one signal to the group ends both, and only "
                     "them\n");

    ksft_test_result(setpgid(1, 0) == -1 && errno == ESRCH,
                     "a process that is not the caller's child is left be\n");
    ksft_test_result(kill(-30000, SIGTERM) == -1 && errno == ESRCH,
                     "and a group with nobody in it is ESRCH\n");

    char arr_said[3];
    orphan_stopped(arr_said);
    ksft_test_result(ksft_streq(arr_said, "HT"),
                     "a stopped job its session's leader leaves behind is "
                     "sent SIGHUP and continued, and a terminal's stop does "
                     "nothing to it after\n");
    ksft_finished();
}

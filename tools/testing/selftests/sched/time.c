// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/time.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the clocks, and sleeping, from ring 3.
 */

#include "../kselftest.h"

#include <sys/time.h>

static long ms_of(const struct timespec *p_ts)
{
    return p_ts->tv_sec * 1000 + p_ts->tv_nsec / 1000000;
}

static void caught(int i_sig)
{
    (void)i_sig;
}

int main(void)
{
    struct timespec a;
    struct timespec b;
    struct timeval tv;

    ksft_print_header();
    ksft_set_plan(8);

    clock_gettime(CLOCK_MONOTONIC, &a);
    struct timespec nap = { 0, 100000000 };
    ksft_test_result(nanosleep(&nap, NULL) == 0
                     && clock_gettime(CLOCK_MONOTONIC, &b) == 0
                     && ms_of(&b) - ms_of(&a) >= 100,
                     "nanosleep sleeps at least as long as asked\n");

    struct timespec until;
    clock_gettime(CLOCK_MONOTONIC, &until);
    until.tv_nsec += 50000000;
    if (until.tv_nsec >= 1000000000) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000;
    }
    ksft_test_result(clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until,
                                     NULL) == 0
                     && clock_gettime(CLOCK_MONOTONIC, &b) == 0
                     && ms_of(&b) >= ms_of(&until),
                     "and to a time, with TIMER_ABSTIME\n");

    ksft_test_result(clock_gettime(CLOCK_REALTIME, &a) == 0 && a.tv_sec > 0
                     && gettimeofday(&tv, NULL) == 0
                     && tv.tv_sec - a.tv_sec <= 1 && time(NULL) >= a.tv_sec,
                     "the wall clock reads the same three ways\n");

    ksft_test_result(clock_getres(CLOCK_MONOTONIC, &a) == 0
                     && a.tv_sec == 0 && a.tv_nsec > 0,
                     "and says how finely it counts\n");

    for (volatile long i_spin = 0; i_spin < 20000000; i_spin++) {
    }
    ksft_test_result(clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &a) == 0
                     && ms_of(&a) > 0,
                     "the cpu time a process has spent is counted\n");

    ksft_test_result(clock_gettime(99, &a) == -1 && errno == EINVAL,
                     "a clock there is not is EINVAL\n");

    ksft_test_result(settimeofday(&tv, NULL) == -1 && errno == EPERM,
                     "a guest may not set the time\n");

    /* a signal ends a sleep, and says how much of it was left */
    signal(SIGALRM, caught);
    struct itimerval soon = { .it_value = { 0, 50000 } };
    setitimer(ITIMER_REAL, &soon, NULL);
    struct timespec rem = { 0, 0 };
    nap = (struct timespec){ 2, 0 };
    ksft_test_result(nanosleep(&nap, &rem) == -1 && errno == EINTR
                     && rem.tv_sec >= 1,
                     "a handler cuts a sleep short, with what is left\n");
    ksft_finished();
}

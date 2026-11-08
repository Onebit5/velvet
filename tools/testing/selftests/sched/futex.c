// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/futex.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a futex as one thread can see it: a word, and waits that end.
 */

#include "../kselftest.h"

#include <sys/time.h>

#include <velvet/futex.h>

static long futex(uint32_t *p_word, int i_op, uint32_t u_val,
                  const struct timespec *p_ts, uint32_t *p_word2,
                  uint32_t u_val3)
{
    return syscall(SYS_futex, p_word, i_op, u_val, p_ts, p_word2, u_val3);
}

static long since_ms(const struct timespec *p_then)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - p_then->tv_sec) * 1000
           + (now.tv_nsec - p_then->tv_nsec) / 1000000;
}

static void caught(int i_sig)
{
    (void)i_sig;
}

int main(void)
{
    uint32_t arr_words[2] = { 7, 0 };
    uint32_t *p_word = &arr_words[0];
    struct timespec start;
    struct timespec ts = { 0, 30000000 };

    ksft_print_header();
    ksft_set_plan(8);

    ksft_test_result(futex(p_word, FUTEX_WAIT, 8, NULL, NULL, 0) == -1
                     && errno == EAGAIN,
                     "a wait on a word that no longer says wait is over\n");
    ksft_test_result(futex(p_word, FUTEX_WAKE, 1, NULL, NULL, 0) == 0,
                     "and a wake with nobody waiting wakes nobody\n");

    clock_gettime(CLOCK_MONOTONIC, &start);
    ksft_test_result(futex(p_word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 7, &ts,
                           NULL, 0) == -1
                     && errno == ETIMEDOUT && since_ms(&start) >= 25,
                     "a wait with a limit waits that long and says so\n");

    struct timespec at;
    clock_gettime(CLOCK_MONOTONIC, &at);
    at.tv_nsec += 30000000;
    if (at.tv_nsec >= 1000000000) {
        at.tv_sec++;
        at.tv_nsec -= 1000000000;
    }
    clock_gettime(CLOCK_MONOTONIC, &start);
    ksft_test_result(futex(p_word, FUTEX_WAIT_BITSET, 7, &at, NULL,
                           FUTEX_BITSET_MATCH_ANY) == -1
                     && errno == ETIMEDOUT && since_ms(&start) >= 25,
                     "and a bitset wait until a moment on the clock\n");

    ksft_test_result(futex((uint32_t *)((char *)p_word + 1), FUTEX_WAIT, 7,
                           NULL, NULL, 0) == -1 && errno == EINVAL
                     && futex(p_word, FUTEX_WAIT_BITSET, 7, NULL, NULL, 0)
                        == -1 && errno == EINVAL,
                     "a word is aligned, and a bitset of none is nothing\n");

    ksft_test_result(futex(p_word, FUTEX_CMP_REQUEUE, 1, (void *)1,
                           &arr_words[1], 8) == -1 && errno == EAGAIN
                     && futex(p_word, FUTEX_CMP_REQUEUE, 1, (void *)1,
                              &arr_words[1], 7) == 0,
                     "a requeue that checks the word moves nobody when it "
                     "has changed\n");

    /* a signal, with a handler that does not ask for the call again */
    struct sigaction act;
    memset(&act, 0, sizeof act);
    act.sa_handler = caught;
    sigaction(SIGALRM, &act, NULL);
    struct itimerval soon = { { 0, 0 }, { 0, 30000 } };
    setitimer(ITIMER_REAL, &soon, NULL);
    ksft_test_result(futex(p_word, FUTEX_WAIT, 7, NULL, NULL, 0) == -1
                     && errno == EINTR,
                     "a signal ends a wait, its handler run\n");

    /* and one that does, for a wait with no limit */
    act.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &act, NULL);
    setitimer(ITIMER_REAL, &soon, NULL);
    struct timespec longer = { 0, 200000000 };
    clock_gettime(CLOCK_MONOTONIC, &start);
    ksft_test_result(futex(p_word, FUTEX_WAIT, 7, &longer, NULL, 0) == -1
                     && errno == EINTR && since_ms(&start) < 150,
                     "and one with a limit is not made again after a "
                     "handler, SA_RESTART or not\n");
    ksft_finished();
}

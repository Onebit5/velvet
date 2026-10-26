// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/select.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * poll and select, from ring 3: waiting on several files at once.
 */

#include "../kselftest.h"

#include <poll.h>
#include <sys/select.h>

static volatile sig_atomic_t s_i_caught;

static void caught(int i_sig)
{
    s_i_caught = i_sig;
}

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void)
{
    int arr_fds[2];
    struct pollfd arr_poll[2];
    fd_set reads;

    ksft_print_header();
    ksft_set_plan(9);

    pipe(arr_fds);
    arr_poll[0] = (struct pollfd){ .fd = arr_fds[0], .events = POLLIN };
    long i_from = now_ms();
    ksft_test_result(poll(arr_poll, 1, 100) == 0 && arr_poll[0].revents == 0
                     && now_ms() - i_from >= 100,
                     "an empty pipe is not ready, until the time is up\n");

    write(arr_fds[1], "x", 1);
    arr_poll[1] = (struct pollfd){ .fd = arr_fds[1], .events = POLLOUT };
    ksft_test_result(poll(arr_poll, 2, -1) == 2
                     && (arr_poll[0].revents & POLLIN)
                     && (arr_poll[1].revents & POLLOUT),
                     "something in it is POLLIN, and room in it POLLOUT\n");

    FD_ZERO(&reads);
    FD_SET(arr_fds[0], &reads);
    struct timeval tv = { 0, 0 };
    ksft_test_result(select(arr_fds[0] + 1, &reads, NULL, NULL, &tv) == 1
                     && FD_ISSET(arr_fds[0], &reads),
                     "select says the same\n");

    char c_ch;
    read(arr_fds[0], &c_ch, 1);
    FD_ZERO(&reads);
    FD_SET(arr_fds[0], &reads);
    tv = (struct timeval){ 0, 50000 };
    ksft_test_result(select(arr_fds[0] + 1, &reads, NULL, NULL, &tv) == 0
                     && !FD_ISSET(arr_fds[0], &reads),
                     "and nothing ready when it is empty again\n");

    close(arr_fds[1]);
    ksft_test_result(poll(arr_poll, 1, 0) == 1
                     && (arr_poll[0].revents & POLLHUP),
                     "a pipe nobody can write to is POLLHUP\n");

    struct pollfd bad = { .fd = 99, .events = POLLIN };
    struct pollfd none = { .fd = -1, .events = POLLIN };
    ksft_test_result(poll(&bad, 1, 0) == 1 && bad.revents == POLLNVAL
                     && poll(&none, 1, 0) == 0,
                     "a descriptor not open is POLLNVAL, a negative one "
                     "nothing\n");
    close(arr_fds[0]);

    /* a signal ends the wait, and a handler runs for it */
    signal(SIGUSR1, caught);
    pid_t i_parent = getpid();
    pid_t i_pid = fork();
    if (i_pid == 0) {
        ksft_sleep_ms(100);
        kill(i_parent, SIGUSR1);
        _exit(0);
    }
    pipe(arr_fds);
    arr_poll[0] = (struct pollfd){ .fd = arr_fds[0], .events = POLLIN };
    ksft_test_result(poll(arr_poll, 1, 5000) == -1 && errno == EINTR
                     && s_i_caught == SIGUSR1,
                     "a signal ends a poll, as EINTR\n");
    waitpid(i_pid, NULL, 0);

    /* ppoll lets through what its mask does, for the wait only */
    sigset_t set;
    sigset_t was;
    sigset_t open_mask;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, &was);
    sigemptyset(&open_mask);
    s_i_caught = 0;
    i_pid = fork();
    if (i_pid == 0) {
        ksft_sleep_ms(100);
        kill(i_parent, SIGUSR1);
        _exit(0);
    }
    struct timespec ts = { 5, 0 };
    bool b_ended = ppoll(arr_poll, 1, &ts, &open_mask) == -1
                   && errno == EINTR && s_i_caught == SIGUSR1;
    sigset_t now;
    sigprocmask(SIG_SETMASK, NULL, &now);
    ksft_test_result(b_ended && sigismember(&now, SIGUSR1),
                     "ppoll waits under its own mask, and the old one "
                     "comes back\n");
    waitpid(i_pid, NULL, 0);

    s_i_caught = 0;
    raise(SIGUSR1);
    ts = (struct timespec){ 0, 20000000 };
    ksft_test_result(pselect(0, NULL, NULL, NULL, &ts, &open_mask) == -1
                     && errno == EINTR && s_i_caught == SIGUSR1,
                     "and pselect the same, for one already waiting\n");
    sigprocmask(SIG_SETMASK, &was, NULL);
    ksft_finished();
}

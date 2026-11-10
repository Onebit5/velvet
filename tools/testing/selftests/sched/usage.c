// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/usage.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a process has had of the cpus, as getrusage, times and the cpu
 * clocks tell it: user time apart from system time, its threads', its
 * children's, and how often it gave a cpu up.
 */

#define _GNU_SOURCE
#include "../kselftest.h"

#include <pthread.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/times.h>

/* how long anything here may take before it counts as not happening */
#define PATIENCE_MS 5000

/* what a buffer of the kernel's work is, read from /dev/zero */
#define ZEROES (256 * 1024)

static char s_arr_zeroes[ZEROES];

static long ms_of_tv(struct timeval tv)
{
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static long ms_of_ts(struct timespec ts)
{
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ms_of_ts(ts);
}

static long clock_ms(clockid_t i_clock)
{
    struct timespec ts = { 0, 0 };
    clock_gettime(i_clock, &ts);
    return ms_of_ts(ts);
}

static struct rusage usage_of(int i_who)
{
    struct rusage ru;
    memset(&ru, 0, sizeof ru);
    getrusage(i_who, &ru);
    return ru;
}

/* ring 3 until `who` has spent l_ms there, or patience runs out */
static bool spin_user(int i_who, long l_ms)
{
    long l_until = now_ms() + PATIENCE_MS;
    while (ms_of_tv(usage_of(i_who).ru_utime) < l_ms) {
        for (volatile int i_spin = 0; i_spin < 200000; i_spin++) {
        }
        if (now_ms() > l_until) {
            return false;
        }
    }
    return true;
}

/* the kernel's time, zeroing a buffer, until it is l_ms more than it was */
static bool spin_system(long l_ms)
{
    int i_fd = open("/dev/zero", O_RDONLY);
    long l_from = ms_of_tv(usage_of(RUSAGE_SELF).ru_stime);
    long l_until = now_ms() + PATIENCE_MS;
    bool b_reached = false;
    while (i_fd >= 0 && !b_reached && now_ms() <= l_until) {
        for (int i_read = 0; i_read < 16; i_read++) {
            if (read(i_fd, s_arr_zeroes, sizeof s_arr_zeroes) < 0) {
                close(i_fd);
                return false;
            }
        }
        b_reached = ms_of_tv(usage_of(RUSAGE_SELF).ru_stime) >= l_from + l_ms;
    }
    if (i_fd >= 0) {
        close(i_fd);
    }
    return b_reached;
}

/*
 * a second thread spins, says so, and waits to be let go, so its time
 * can be asked for while it is alive and again once it has ended
 */
static atomic_int s_i_phase;
static bool s_b_spun;

static void *spinner(void *p_arg)
{
    (void)p_arg;
    s_b_spun = spin_user(RUSAGE_THREAD, 200);
    atomic_store(&s_i_phase, 1);
    while (atomic_load(&s_i_phase) != 2) {
        usleep(1000);
    }
    return NULL;
}

static bool wait_phase(int i_phase)
{
    long l_until = now_ms() + PATIENCE_MS;
    while (atomic_load(&s_i_phase) != i_phase) {
        if (now_ms() > l_until) {
            return false;
        }
        usleep(1000);
    }
    return true;
}

int main(void)
{
    ksft_print_header();
    ksft_set_plan(11);

    ksft_test_result(spin_user(RUSAGE_SELF, 100),
                     "time spent in ring 3 is user time\n");

    ksft_test_result(spin_system(50),
                     "time the kernel spends on a call is system time\n");

    /* a live thread's time is its process's, and only its own */
    struct rusage before = usage_of(RUSAGE_THREAD);
    pthread_t thread;
    bool b_started = pthread_create(&thread, NULL, spinner, NULL) == 0;
    bool b_spun = b_started && wait_phase(1) && s_b_spun;
    struct rusage self = usage_of(RUSAGE_SELF);
    struct rusage mine = usage_of(RUSAGE_THREAD);
    ksft_test_result(b_spun && ms_of_tv(self.ru_utime)
                               >= ms_of_tv(mine.ru_utime) + 150,
                     "a process's time has its live threads' in it\n");
    ksft_test_result(b_spun && ms_of_tv(mine.ru_utime)
                               < ms_of_tv(before.ru_utime) + 100,
                     "and a thread's is only its own\n");
    atomic_store(&s_i_phase, 2);
    if (b_started) {
        pthread_join(thread, NULL);
    }
    struct rusage after = usage_of(RUSAGE_SELF);
    ksft_test_result(b_spun && ms_of_tv(after.ru_utime)
                               >= ms_of_tv(self.ru_utime),
                     "and stays its process's once the thread has ended\n");

    /* the same time, told by the cpu clocks and by times */
    self = usage_of(RUSAGE_SELF);
    long l_process = clock_ms(CLOCK_PROCESS_CPUTIME_ID);
    long l_thread = clock_ms(CLOCK_THREAD_CPUTIME_ID);
    ksft_test_result(l_process >= l_thread && l_process + 10
                     >= ms_of_tv(self.ru_utime) + ms_of_tv(self.ru_stime),
                     "the cpu clocks tell the same time getrusage does\n");

    struct tms counts;
    self = usage_of(RUSAGE_SELF);
    clock_t i_since = times(&counts);
    long l_tick_ms = 1000 / sysconf(_SC_CLK_TCK);
    ksft_test_result(i_since != (clock_t)-1 && counts.tms_stime > 0
                     && labs(counts.tms_utime * l_tick_ms
                             - ms_of_tv(self.ru_utime)) <= 20
                     && labs(counts.tms_stime * l_tick_ms
                             - ms_of_tv(self.ru_stime)) <= 20,
                     "and times tells it in clock ticks\n");

    /* a child's time, once it has been waited for */
    pid_t i_child = fork();
    if (i_child == 0) {
        _exit(spin_user(RUSAGE_SELF, 100) ? 0 : 1);
    }
    int i_status = -1;
    struct rusage child;
    memset(&child, 0, sizeof child);
    bool b_waited = i_child > 0 && wait4(i_child, &i_status, 0, &child)
                                   == i_child
                    && WIFEXITED(i_status) && WEXITSTATUS(i_status) == 0;
    ksft_test_result(b_waited && ms_of_tv(child.ru_utime) >= 100,
                     "wait4 tells the time a child ran for\n");
    struct rusage children = usage_of(RUSAGE_CHILDREN);
    times(&counts);
    ksft_test_result(b_waited && ms_of_tv(children.ru_utime) >= 100
                     && counts.tms_cutime * l_tick_ms >= 100,
                     "and so do RUSAGE_CHILDREN and times, after\n");

    /* each sleep is a cpu given up */
    long l_gave = usage_of(RUSAGE_SELF).ru_nvcsw;
    for (int i_nap = 0; i_nap < 5; i_nap++) {
        struct timespec nap = { 0, 20000000 };
        nanosleep(&nap, NULL);
    }
    ksft_test_result(usage_of(RUSAGE_SELF).ru_nvcsw >= l_gave + 5,
                     "a sleep is a switch the process chose\n");

    struct rusage none;
    ksft_test_result(getrusage(7, &none) == -1 && errno == EINVAL,
                     "a who there is not is EINVAL\n");
    ksft_finished();
}

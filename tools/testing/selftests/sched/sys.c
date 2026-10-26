// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/sys.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * who a process is, what it may use and what the machine is, from ring 3.
 */

#include "../kselftest.h"

#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/times.h>
#include <sys/utsname.h>

int main(void)
{
    struct utsname name;
    struct rlimit limit;
    struct sysinfo info;
    char arr_a[32];
    char arr_b[32];
    int i_status = 0;

    ksft_print_header();
    ksft_set_plan(12);

    ksft_test_result(uname(&name) == 0 && ksft_streq(name.sysname, "velvet")
                     && ksft_streq(name.machine, "x86_64")
                     && name.release[0] != '\0',
                     "uname says which kernel, on what\n");
    ksft_test_result(sethostname("guest", 5) == -1 && errno == EPERM,
                     "and only root may name the machine\n");

    ksft_test_result(getrandom(arr_a, sizeof arr_a, 0) == sizeof arr_a
                     && getrandom(arr_b, sizeof arr_b, GRND_NONBLOCK)
                        == sizeof arr_b
                     && memcmp(arr_a, arr_b, sizeof arr_a) != 0,
                     "getrandom never hands out the same bytes twice\n");

    ksft_test_result(getrlimit(RLIMIT_NOFILE, &limit) == 0
                     && limit.rlim_cur == 64 && limit.rlim_max == 64,
                     "a process may have 64 descriptors open\n");
    limit.rlim_cur = 8;
    ksft_test_result(setrlimit(RLIMIT_NOFILE, &limit) == 0
                     && dup2(0, 9) == -1 && errno == EBADF
                     && dup(0) >= 0,
                     "and fewer, when it lowers its own limit\n");
    limit.rlim_cur = 64;
    setrlimit(RLIMIT_NOFILE, &limit);
    limit.rlim_max = 1000;
    ksft_test_result(setrlimit(RLIMIT_NOFILE, &limit) == -1
                     && errno == EPERM,
                     "but may not raise the hard one\n");

    ksft_test_result(sysinfo(&info) == 0 && info.totalram > 0
                     && info.freeram > 0 && info.freeram <= info.totalram
                     && info.procs > 0,
                     "sysinfo says how much memory there is, and free\n");

    for (volatile long i_spin = 0; i_spin < 20000000; i_spin++) {
    }
    struct tms counts;
    struct rusage usage;
    ksft_test_result(times(&counts) > 0 && getrusage(RUSAGE_SELF, &usage) == 0
                     && (usage.ru_utime.tv_sec > 0
                         || usage.ru_utime.tv_usec > 0),
                     "and a process the time it has run\n");

    ksft_test_result(prctl(PR_SET_NAME, "renamed") == 0
                     && prctl(PR_GET_NAME, arr_a) == 0
                     && ksft_streq(arr_a, "renamed"),
                     "PR_SET_NAME renames it, and PR_GET_NAME says so\n");

    ksft_test_result(getppid() == 1 && getpgid(0) == getpid()
                     && getsid(0) == 1,
                     "a test is init's child, its own group, in init's "
                     "session\n");
    ksft_test_result(setsid() == -1 && errno == EPERM,
                     "and leading its group, may not start a session\n");

    pid_t i_pid = fork();
    if (i_pid == 0) {
        pid_t i_me = getpid();
        _exit(setsid() == i_me && getsid(0) == i_me && getpgid(0) == i_me
              ? 0 : 1);
    }
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, 0),
                     "while a child of it may, leading both\n");
    ksft_finished();
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/procstat.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what /proc says of the machine as a whole, held against what a
 * program can see of it for itself.
 */

#define _GNU_SOURCE
#include "../kselftest.h"

#include <limits.h>
#include <sched.h>
#include <sys/sysinfo.h>

#define TEXT_MAX 16384

static char s_arr_text[TEXT_MAX];

static int64_t slurp(const char *str_path)
{
    int64_t i_len = ksft_read_file(str_path, s_arr_text, TEXT_MAX - 1);
    s_arr_text[(i_len > 0) ? i_len : 0] = '\0';
    return i_len;
}

/* the number after `str_name ` at the start of a line of stat, or -1 */
static long long counted(const char *str_name)
{
    size_t sz_name = strlen(str_name);
    for (const char *p_line = s_arr_text; p_line != NULL && *p_line != '\0';
         p_line = strchr(p_line, '\n'), p_line = p_line ? p_line + 1 : NULL) {
        if (strncmp(p_line, str_name, sz_name) == 0
            && p_line[sz_name] == ' ') {
            return strtoll(p_line + sz_name + 1, NULL, 10);
        }
    }
    return -1;
}

/*
 * a cpu line's ten numbers added up, into *p_sum, and its user time into
 * *p_user: false for a line that does not have ten
 */
static bool cpu_line(const char *str_line, long long *p_sum,
                     long long *p_user)
{
    const char *p_at = strchr(str_line, ' ');
    int i_fields = 0;
    *p_sum = 0;
    while (p_at != NULL && *p_at == ' ' && i_fields < 10) {
        while (*p_at == ' ') {
            p_at++;
        }
        char *p_end;
        long long i_value = strtoll(p_at, &p_end, 10);
        if (p_end == p_at) {
            break;
        }
        *p_user = (i_fields == 0) ? i_value : *p_user;
        *p_sum += i_value;
        i_fields++;
        p_at = p_end;
    }
    return i_fields == 10 && (p_at == NULL || *p_at == '\n');
}

/* the stat lines for the cores: how many, and their sums, all and each */
static int cores_in_stat(long long *p_all, long long *p_each,
                         long long *p_user)
{
    int i_cores = 0;
    long long i_sum;
    long long i_user = 0;
    *p_all = -1;
    *p_each = 0;
    for (const char *p_line = s_arr_text; p_line != NULL && *p_line != '\0';
         p_line = strchr(p_line, '\n'), p_line = p_line ? p_line + 1 : NULL) {
        if (strncmp(p_line, "cpu  ", 5) == 0 && cpu_line(p_line, &i_sum,
                                                          &i_user)) {
            *p_all = i_sum;
            *p_user = i_user;
        } else if (strncmp(p_line, "cpu", 3) == 0 && p_line[3] >= '0'
                   && p_line[3] <= '9' && cpu_line(p_line, &i_sum, &i_user)) {
            *p_each += i_sum;
            i_cores++;
        }
    }
    return i_cores;
}

static void check_stat(void)
{
    long long i_all;
    long long i_each;
    long long i_user;
    int64_t i_len = slurp("/proc/stat");
    int i_cores = cores_in_stat(&i_all, &i_each, &i_user);
    long long i_slack = 10LL * i_cores;
    ksft_test_result(i_len > 0 && i_cores >= 1 && i_all > 0
                     && llabs(i_all - i_each) <= i_slack + i_all / 100,
                     "stat has a line for the cores and one for each, "
                     "adding up\n");

    long long i_switches = counted("ctxt");
    long long i_made = counted("processes");
    long long i_booted = counted("btime");
    long long i_running = counted("procs_running");
    ksft_test_result(counted("intr") >= 0 && i_switches > 0 && i_made > 0
                     && i_running >= 1 && counted("procs_blocked") >= 0,
                     "and the machine's counts\n");

    struct sysinfo info;
    sysinfo(&info);
    long long i_now = (long long)time(NULL);
    ksft_test_result(i_booted > 0
                     && llabs(i_booted + (long long)info.uptime - i_now)
                        <= 2,
                     "it came up when the clock and the uptime say\n");

    /* a sleep is a switch away, and a fork is one more made */
    ksft_sleep_ms(30);
    pid_t i_child = fork();
    if (i_child == 0) {
        _exit(0);
    }
    waitpid(i_child, NULL, 0);
    for (volatile long i_spin = 0; i_spin < 30000000; i_spin++) {
    }
    long long i_user_before = i_user;
    slurp("/proc/stat");
    cores_in_stat(&i_all, &i_each, &i_user);
    ksft_test_result(counted("ctxt") > i_switches
                     && counted("processes") > i_made
                     && i_user > i_user_before,
                     "and counts on: switches, forks, and time in ring 3\n");
}

/* a load as written, a whole and two digits: hundredths, or -1 */
static long load_at(const char **pp_at)
{
    char *p_end;
    long i_whole = strtol(*pp_at, &p_end, 10);
    if (p_end == *pp_at || *p_end != '.' || p_end[1] < '0' || p_end[1] > '9'
        || p_end[2] < '0' || p_end[2] > '9' || p_end[3] != ' ') {
        return -1;
    }
    *pp_at = p_end + 4;
    return i_whole * 100 + (p_end[1] - '0') * 10 + (p_end[2] - '0');
}

static void check_loadavg(void)
{
    int64_t i_len = slurp("/proc/loadavg");
    const char *p_at = s_arr_text;
    long arr_load[3];
    for (int i_at = 0; i_at < 3; i_at++) {
        arr_load[i_at] = load_at(&p_at);
    }
    char *p_end;
    long i_running = strtol(p_at, &p_end, 10);
    long i_threads = (*p_end == '/') ? strtol(p_end + 1, &p_end, 10) : -1;
    long i_last = (*p_end == ' ') ? strtol(p_end + 1, &p_end, 10) : -1;
    ksft_test_result(i_len > 0 && arr_load[0] >= 0 && arr_load[1] >= 0
                     && arr_load[2] >= 0 && *p_end == '\n'
                     && i_running >= 1 && i_threads >= i_running
                     && i_last >= getpid(),
                     "loadavg is three loads, the threads that want a core"
                     " of all there are, and the last pid\n");
    struct sysinfo info;
    sysinfo(&info);
    long i_from_sysinfo = (long)((info.loads[0] * 100 + 32768) >> 16);
    ksft_test_result(labs(i_from_sysinfo - arr_load[0]) <= 2,
                     "and sysinfo says the same load\n");

    /*
     * busy, the minute's load is above nothing by the next time the
     * count is folded in, which is every five seconds
     */
    struct timespec start;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    long i_busy = 0;
    do {
        for (volatile long i_spin = 0; i_spin < 2000000; i_spin++) {
        }
        slurp("/proc/loadavg");
        p_at = s_arr_text;
        i_busy = load_at(&p_at);
        clock_gettime(CLOCK_MONOTONIC, &now);
    } while (i_busy <= 0 && now.tv_sec - start.tv_sec < 12);
    ksft_test_result(i_busy > 0, "a busy machine has a load\n");
}

/* uptime's two figures, how long it has been up and idle: false for none */
static bool uptime_of(double *p_up, double *p_idle)
{
    int64_t i_len = slurp("/proc/uptime");
    char *p_end;
    *p_up = strtod(s_arr_text, &p_end);
    *p_idle = (*p_end == ' ') ? strtod(p_end + 1, &p_end) : -1;
    return i_len > 0 && *p_end == '\n' && *p_up > 0 && *p_idle >= 0;
}

static void check_uptime(void)
{
    double d_up;
    double d_idle;
    double d_later_up;
    double d_later_idle;
    struct sysinfo info;
    bool b_read = uptime_of(&d_up, &d_idle);
    sysinfo(&info);
    ksft_test_result(b_read && labs((long)d_up - (long)info.uptime) <= 1,
                     "uptime is how long it has been up\n");
    /* the cores with nothing to do while this sleeps are idle meanwhile */
    ksft_sleep_ms(300);
    ksft_test_result(uptime_of(&d_later_up, &d_later_idle)
                     && d_later_up > d_up && d_later_idle > d_idle,
                     "and how long the cores have been idle, going on\n");
}

/* how many cores a list such as 0-3,6 names */
static int cores_listed(const char *str_list)
{
    int i_count = 0;
    const char *p_at = str_list;
    while (*p_at >= '0' && *p_at <= '9') {
        char *p_end;
        long i_from = strtol(p_at, &p_end, 10);
        long i_to = (*p_end == '-') ? strtol(p_end + 1, &p_end, 10) : i_from;
        i_count += (int)(i_to - i_from + 1);
        p_at = (*p_end == ',') ? p_end + 1 : p_end;
    }
    return i_count;
}

/*
 * the cores a thread may run on are the ones its status lists, all of
 * them taking work, and what a libc counts as the processors there are
 */
static void check_affinity(void)
{
    cpu_set_t set;
    long long i_all;
    long long i_each;
    long long i_user;
    CPU_ZERO(&set);
    bool b_got = sched_getaffinity(0, sizeof set, &set) == 0;
    int i_mine = CPU_COUNT(&set);
    slurp("/proc/stat");
    int i_cores = cores_in_stat(&i_all, &i_each, &i_user);
    slurp("/proc/self/status");
    const char *str_list = strstr(s_arr_text, "\nCpus_allowed_list:\t");
    int i_listed = (str_list != NULL) ? cores_listed(str_list + 20) : -1;
    ksft_test_result(b_got && i_mine >= 1 && i_mine == i_listed
                     && i_mine <= i_cores
                     && sysconf(_SC_NPROCESSORS_ONLN) == i_mine,
                     "a thread may run on the cores its status lists, which"
                     " a libc counts\n");
    errno = 0;
    long i_short = syscall(SYS_sched_getaffinity, 0, 4, &set);
    int i_short_errno = errno;
    errno = 0;
    long i_nobody = syscall(SYS_sched_getaffinity, INT_MAX, sizeof set, &set);
    ksft_test_result(i_short == -1 && i_short_errno == EINVAL
                     && i_nobody == -1 && errno == ESRCH,
                     "and nobody's, or into too little room, is an error\n");
}

int main(void)
{
    ksft_print_header();
    ksft_set_plan(11);
    check_stat();
    check_loadavg();
    check_uptime();
    check_affinity();
    ksft_finished();
}

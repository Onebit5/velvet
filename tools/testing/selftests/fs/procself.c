// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/procself.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a process's own directory in /proc, held against what the process
 * knows of itself.
 */

#define _GNU_SOURCE
#include "../kselftest.h"

#include <dirent.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>

#define TEXT_MAX 8192

static char s_arr_text[TEXT_MAX];
static char s_arr_want[TEXT_MAX];

/* the environment as exec gave it, before anything here could change it */
static char **s_pp_env;

/* a file of /proc read whole and ended by a NUL: its length, or -1 */
static int64_t slurp(const char *str_path)
{
    int64_t i_len = ksft_read_file(str_path, s_arr_text, TEXT_MAX - 1);
    s_arr_text[(i_len > 0) ? i_len : 0] = '\0';
    return i_len;
}

/* where a link leads, into s_arr_text, or "" */
static const char *link_of(const char *str_path)
{
    ssize_t i_len = readlink(str_path, s_arr_text, TEXT_MAX - 1);
    s_arr_text[(i_len > 0) ? i_len : 0] = '\0';
    return s_arr_text;
}

/*
 * field i_field of a stat line, counted from 1 the way the fields are
 * documented, the name in brackets being the second however many spaces
 * it has: what is after the last ")" is split at single spaces
 */
static long long stat_field(const char *str_line, int i_field)
{
    const char *p_at = strrchr(str_line, ')');
    if (i_field < 3 || p_at == NULL) {
        return -1;
    }
    p_at += 2;
    for (int i_at = 3; i_at < i_field; i_at++) {
        p_at = strchr(p_at, ' ');
        if (p_at == NULL) {
            return -1;
        }
        p_at++;
    }
    return (*p_at == '-' || (*p_at >= '0' && *p_at <= '9'))
           ? strtoll(p_at, NULL, 10) : -1;
}

/* how many fields a stat line has */
static int stat_fields(const char *str_line)
{
    const char *p_at = strrchr(str_line, ')');
    int i_count = 2;
    for (; p_at != NULL && *p_at != '\0' && *p_at != '\n'; p_at++) {
        i_count += *p_at == ' ';
    }
    return i_count;
}

/* whether a directory lists str_name */
static bool lists(const char *str_dir, const char *str_name)
{
    DIR *p_dir = opendir(str_dir);
    bool b_found = false;
    struct dirent *p_entry;
    while (p_dir != NULL && !b_found && (p_entry = readdir(p_dir)) != NULL) {
        b_found = ksft_streq(p_entry->d_name, str_name);
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    return b_found;
}

/* how many entries a directory has, . and .. aside */
static int entries_in(const char *str_dir)
{
    DIR *p_dir = opendir(str_dir);
    int i_count = 0;
    struct dirent *p_entry;
    while (p_dir != NULL && (p_entry = readdir(p_dir)) != NULL) {
        i_count += p_entry->d_name[0] != '.';
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    return (p_dir != NULL) ? i_count : -1;
}

/* a second thread, there until it is told to go */
static atomic_int s_i_go;
static atomic_int s_i_tid;

static void *waiter(void *p_arg)
{
    (void)p_arg;
    atomic_store(&s_i_tid, (int)syscall(SYS_gettid));
    while (atomic_load(&s_i_go) == 0) {
        ksft_sleep_ms(5);
    }
    return NULL;
}

/* the maps line whose range holds u_at: its text, or NULL */
static const char *maps_line(uint64_t u_at)
{
    for (char *p_line = s_arr_text; *p_line != '\0'; ) {
        char *p_end = strchr(p_line, '\n');
        unsigned long long u_from = strtoull(p_line, NULL, 16);
        const char *p_dash = strchr(p_line, '-');
        unsigned long long u_to = (p_dash != NULL)
                                  ? strtoull(p_dash + 1, NULL, 16) : 0;
        if (u_at >= u_from && u_at < u_to) {
            if (p_end != NULL) {
                *p_end = '\0';
            }
            return p_line;
        }
        if (p_end == NULL) {
            break;
        }
        p_line = p_end + 1;
    }
    return NULL;
}

/* what the line names, after its five fields and the padding, or "" */
static const char *mapped(const char *str_line)
{
    const char *p_at = str_line;
    for (int i_field = 0; p_at != NULL && i_field < 5; i_field++) {
        p_at = strchr(p_at, ' ');
        while (p_at != NULL && *p_at == ' ') {
            p_at++;
        }
    }
    return (p_at != NULL) ? p_at : "";
}

static void check_links(void)
{
    char arr_pid[32];
    char arr_thread[64];
    snprintf(arr_pid, sizeof arr_pid, "%d", getpid());
    snprintf(arr_thread, sizeof arr_thread, "%d/task/%ld", getpid(),
             (long)syscall(SYS_gettid));
    struct stat st_link;
    struct stat st_dir;
    ksft_test_result(ksft_streq(link_of("/proc/self"), arr_pid)
                     && lstat("/proc/self", &st_link) == 0
                     && S_ISLNK(st_link.st_mode)
                     && stat("/proc/self", &st_dir) == 0
                     && S_ISDIR(st_dir.st_mode)
                     && st_dir.st_uid == getuid(),
                     "/proc/self is a link to the process's directory\n");
    ksft_test_result(ksft_streq(link_of("/proc/thread-self"), arr_thread),
                     "and /proc/thread-self to its thread's\n");
    ksft_test_result(lists("/proc", arr_pid),
                     "the process is listed in /proc\n");
}

static void check_stat(char *str_argv0)
{
    int64_t i_len = slurp("/proc/self/stat");
    const char *str_base = strrchr(str_argv0, '/');
    str_base = (str_base != NULL) ? str_base + 1 : str_argv0;
    snprintf(s_arr_want, sizeof s_arr_want, "%d (%.15s) R ", getpid(),
             str_base);
    ksft_test_result(i_len > 0 && strncmp(s_arr_text, s_arr_want,
                                          strlen(s_arr_want)) == 0
                     && stat_fields(s_arr_text) == 52,
                     "stat starts with the pid, the name and R, and has "
                     "fifty-two fields\n");
    ksft_test_result(stat_field(s_arr_text, 4) == getppid()
                     && stat_field(s_arr_text, 5) == getpgrp()
                     && stat_field(s_arr_text, 6) == getsid(0)
                     && stat_field(s_arr_text, 20) == 1,
                     "its parent, group, session and one thread\n");
    long long i_start = stat_field(s_arr_text, 22);
    long long i_vsize = stat_field(s_arr_text, 23);
    double d_up = 0;
    char arr_up[64];
    if (ksft_read_file("/proc/uptime", arr_up, sizeof arr_up - 1) > 0) {
        d_up = strtod(arr_up, NULL);
    }
    /*
     * not its rss, which some kernels count a core at a time and add up
     * only now and then, so a small program may read 0 there
     */
    ksft_test_result(i_start >= 0 && i_start <= (long long)(d_up * 100) + 100
                     && i_vsize > 0,
                     "it started since boot, and has memory\n");

    i_len = slurp("/proc/self/statm");
    long long i_size = strtoll(s_arr_text, NULL, 10);
    char *p_after = strchr(s_arr_text, ' ');
    long long i_resident = (p_after != NULL) ? strtoll(p_after, NULL, 10)
                                             : -1;
    ksft_test_result(i_len > 0 && i_size >= i_resident && i_resident > 0,
                     "statm has all its pages, and fewer there\n");
}

static void check_status(void)
{
    int64_t i_len = slurp("/proc/self/status");
    snprintf(s_arr_want, sizeof s_arr_want,
             "\nPid:\t%d\nPPid:\t%d\n", getpid(), getppid());
    bool b_ids = ksft_contains(s_arr_text, i_len, s_arr_want);
    snprintf(s_arr_want, sizeof s_arr_want, "\nUid:\t%d\t%d\t", getuid(),
             geteuid());
    b_ids = b_ids && ksft_contains(s_arr_text, i_len, s_arr_want);
    snprintf(s_arr_want, sizeof s_arr_want, "\nTgid:\t%d\n", getpid());
    ksft_test_result(i_len > 0 && strncmp(s_arr_text, "Name:\t", 6) == 0
                     && b_ids && ksft_contains(s_arr_text, i_len, s_arr_want)
                     && ksft_contains(s_arr_text, i_len, "\nThreads:\t1\n")
                     && ksft_contains(s_arr_text, i_len, "\nVmRSS:\t"),
                     "status says who it is, whose, and its memory\n");
}

static void check_strings(int i_argc, char **pp_argv)
{
    size_t sz_want = 0;
    for (int i_at = 0; i_at < i_argc; i_at++) {
        size_t sz_arg = strlen(pp_argv[i_at]) + 1;
        memcpy(s_arr_want + sz_want, pp_argv[i_at], sz_arg);
        sz_want += sz_arg;
    }
    int64_t i_len = slurp("/proc/self/cmdline");
    ksft_test_result(i_len == (int64_t)sz_want
                     && memcmp(s_arr_text, s_arr_want, sz_want) == 0,
                     "cmdline is the arguments, each ended by a NUL\n");

    sz_want = 0;
    for (char **pp_at = s_pp_env; *pp_at != NULL; pp_at++) {
        size_t sz_var = strlen(*pp_at) + 1;
        memcpy(s_arr_want + sz_want, *pp_at, sz_var);
        sz_want += sz_var;
    }
    i_len = slurp("/proc/self/environ");
    struct stat st;
    ksft_test_result(i_len == (int64_t)sz_want
                     && memcmp(s_arr_text, s_arr_want, sz_want) == 0
                     && stat("/proc/self/environ", &st) == 0
                     && (st.st_mode & 0777) == 0400,
                     "environ the environment, for its owner only\n");

    char arr_name[16];
    prctl(PR_GET_NAME, arr_name);
    snprintf(s_arr_want, sizeof s_arr_want, "%s\n", arr_name);
    ksft_test_result(slurp("/proc/self/comm") > 0
                     && ksft_streq(s_arr_text, s_arr_want),
                     "comm is the name prctl has\n");
}

static void check_threads(void)
{
    pthread_t thread;
    char arr_path[64];
    char arr_tid[32];
    bool b_started = pthread_create(&thread, NULL, waiter, NULL) == 0;
    while (b_started && atomic_load(&s_i_tid) == 0) {
        ksft_sleep_ms(1);
    }
    int i_tid = atomic_load(&s_i_tid);
    snprintf(arr_tid, sizeof arr_tid, "%d", i_tid);
    snprintf(arr_path, sizeof arr_path, "/proc/self/task/%d/stat", i_tid);
    int64_t i_len = slurp(arr_path);
    ksft_test_result(b_started && entries_in("/proc/self/task") == 2
                     && lists("/proc/self/task", arr_tid) && i_len > 0
                     && atoi(s_arr_text) == i_tid,
                     "task has a directory for each thread, its stat its "
                     "own\n");
    snprintf(arr_path, sizeof arr_path, "/proc/%d", i_tid);
    struct stat st;
    ksft_test_result(b_started && slurp("/proc/self/stat") > 0
                     && stat_field(s_arr_text, 20) == 2
                     && stat(arr_path, &st) == 0 && !lists("/proc", arr_tid),
                     "a thread is counted, and its number found but not "
                     "listed\n");
    atomic_store(&s_i_go, 1);
    if (b_started) {
        pthread_join(thread, NULL);
    }
}

static void check_memory(void)
{
    int i_local = 0;
    int64_t i_len = slurp("/proc/self/maps");
    const char *str_line = (i_len > 0) ? maps_line((uintptr_t)&i_local)
                                       : NULL;
    ksft_test_result(str_line != NULL
                     && ksft_streq(mapped(str_line), "[stack]")
                     && strstr(str_line, " rw") != NULL,
                     "maps has the stack where a local is\n");
    i_len = slurp("/proc/self/maps");
    str_line = (i_len > 0) ? maps_line((uintptr_t)check_memory) : NULL;
    const char *str_name = (str_line != NULL) ? mapped(str_line) : "";
    const char *str_last = strrchr(str_name, '/');
    ksft_test_result(str_line != NULL && strstr(str_line, " r-xp ") != NULL
                     && str_last != NULL && ksft_streq(str_last, "/procself"),
                     "and the program's code where its functions are\n");

    struct rlimit limit;
    getrlimit(RLIMIT_NOFILE, &limit);
    snprintf(s_arr_want, sizeof s_arr_want, "\nMax open files            "
             "%-20llu ", (unsigned long long)limit.rlim_cur);
    i_len = slurp("/proc/self/limits");
    ksft_test_result(i_len > 0 && strncmp(s_arr_text, "Limit ", 6) == 0
                     && ksft_contains(s_arr_text, i_len, s_arr_want),
                     "limits has what getrlimit says\n");
    i_len = slurp("/proc/self/mounts");
    ksft_test_result(i_len > 0 && ksft_contains(s_arr_text, i_len, " / ")
                     && ksft_contains(s_arr_text, i_len, " /proc proc "),
                     "mounts has the root, and /proc as proc\n");
}

int main(int i_argc, char **pp_argv, char **pp_env)
{
    s_pp_env = pp_env;
    ksft_print_header();
    ksft_set_plan(17);
    check_links();
    check_stat(pp_argv[0]);
    check_status();
    check_strings(i_argc, pp_argv);
    check_threads();
    check_memory();
    ksft_finished();
}

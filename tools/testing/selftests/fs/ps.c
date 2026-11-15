// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/ps.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a ps: every process found the way ps finds them, by walking /proc and
 * reading each one's stat, and what that says of the processes this one
 * makes as they sleep, stop, end and are waited for, and of a thread.
 */

#define _GNU_SOURCE
#include "../kselftest.h"

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>

/* what ps shows of a process, out of its stat */
struct seen {
    int i_pid;
    char arr_comm[32];
    char c_state;
    int i_ppid;
};

/* process i_pid's stat, or false for one that has gone */
static bool stat_of(int i_pid, struct seen *p_out)
{
    char arr_path[64];
    char arr_text[1024];
    snprintf(arr_path, sizeof arr_path, "/proc/%d/stat", i_pid);
    int64_t i_len = ksft_read_file(arr_path, arr_text, sizeof arr_text - 1);
    if (i_len <= 0) {
        return false;
    }
    arr_text[i_len] = '\0';

    /* the name is in brackets and may hold anything, so it ends at the last */
    const char *p_open = strchr(arr_text, '(');
    const char *p_close = strrchr(arr_text, ')');
    if (p_open == NULL || p_close == NULL || p_close < p_open) {
        return false;
    }
    size_t sz_comm = (size_t)(p_close - p_open - 1);
    sz_comm = (sz_comm < sizeof p_out->arr_comm) ? sz_comm
                                                 : sizeof p_out->arr_comm - 1;
    memcpy(p_out->arr_comm, p_open + 1, sz_comm);
    p_out->arr_comm[sz_comm] = '\0';
    p_out->i_pid = atoi(arr_text);
    return sscanf(p_close + 1, " %c %d", &p_out->c_state,
                  &p_out->i_ppid) == 2;
}

/*
 * every process /proc lists, as ps finds them: how many, or -1 when one
 * listed under a number says it is another, or is in no state ps knows.
 * whether i_want was among them, and what it said, into p_found
 */
static int walk(int i_want, struct seen *p_found, bool *p_listed)
{
    int i_count = 0;
    bool b_agree = true;
    DIR *p_dir = opendir("/proc");
    *p_listed = false;
    for (struct dirent *p_ent; p_dir != NULL
                               && (p_ent = readdir(p_dir)) != NULL; ) {
        char *p_end;
        long i_pid = strtol(p_ent->d_name, &p_end, 10);
        struct seen one;
        if (*p_end != '\0' || i_pid <= 0 || !stat_of((int)i_pid, &one)) {
            continue;   /* not a process, or one gone since it was listed */
        }
        b_agree = b_agree && one.i_pid == i_pid
                  && strchr("RSDTtZXIWP", one.c_state) != NULL;
        i_count++;
        if (i_pid == i_want) {
            *p_found = one;
            *p_listed = true;
        }
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    return b_agree ? i_count : -1;
}

/* the state process i_pid comes to within three seconds, or what it has */
static char state_becomes(int i_pid, char c_want)
{
    struct seen one = { .c_state = '?' };
    for (int i_try = 0; i_try < 300; i_try++) {
        if (stat_of(i_pid, &one) && one.c_state == c_want) {
            break;
        }
        ksft_sleep_ms(10);
    }
    return one.c_state;
}

static int s_arr_held[2];
static int s_i_thread_tid;

/* a thread that says who it is and waits to be let go */
static void *held(void *p_arg)
{
    char c_byte;
    (void)p_arg;
    __atomic_store_n(&s_i_thread_tid, gettid(), __ATOMIC_RELEASE);
    ssize_t i_got = read(s_arr_held[0], &c_byte, 1);
    (void)i_got;
    return NULL;
}

/* how many entries a directory has that are not . or .. */
static int entries_in(const char *str_dir)
{
    int i_count = 0;
    DIR *p_dir = opendir(str_dir);
    for (struct dirent *p_ent; p_dir != NULL
                               && (p_ent = readdir(p_dir)) != NULL; ) {
        i_count += (p_ent->d_name[0] != '.') ? 1 : 0;
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    return i_count;
}

static void check_threads(void)
{
    pthread_t thread;
    struct seen one;
    bool b_listed = true;
    bool b_made = pipe(s_arr_held) == 0
                  && pthread_create(&thread, NULL, held, NULL) == 0;
    while (b_made && __atomic_load_n(&s_i_thread_tid, __ATOMIC_ACQUIRE) == 0) {
        ksft_sleep_ms(1);
    }
    int i_tasks = entries_in("/proc/self/task");
    char arr_task[64];
    snprintf(arr_task, sizeof arr_task, "/proc/self/task/%d", s_i_thread_tid);
    struct stat st;
    bool b_task = stat(arr_task, &st) == 0 && S_ISDIR(st.st_mode);
    if (b_made) {
        walk(s_i_thread_tid, &one, &b_listed);
        ssize_t i_put = write(s_arr_held[1], "x", 1);
        (void)i_put;
        pthread_join(thread, NULL);
    }
    ksft_test_result(b_made && i_tasks == 2 && b_task && !b_listed,
                     "a thread is in its process's task directory, and is "
                     "not listed as a process\n");
}

int main(void)
{
    struct seen me;
    struct seen one;
    bool b_listed;

    ksft_print_header();
    ksft_set_plan(6);

    int i_count = walk(getpid(), &me, &b_listed);
    ksft_test_result(i_count >= 2 && b_listed && me.c_state == 'R'
                     && me.i_ppid == getppid(),
                     "ps finds the processes by their directories, each "
                     "stat its own, and this one running\n");
    ksft_test_result(stat_of(1, &one) && one.i_ppid == 0,
                     "and the first, whose parent is nobody\n");

    /*
     * a child waiting on a pipe nothing is written to, whose writing end
     * this one holds until the child is gone
     */
    int arr_pipe[2];
    pid_t i_child = (pipe(arr_pipe) == 0) ? fork() : -1;
    if (i_child == 0) {
        char c_byte;
        close(arr_pipe[1]);
        ssize_t i_got = read(arr_pipe[0], &c_byte, 1);
        _exit((int)i_got);
    }
    char c_waiting = (i_child > 0) ? state_becomes(i_child, 'S') : '?';
    bool b_child = i_child > 0 && stat_of(i_child, &one);
    ksft_test_result(c_waiting == 'S' && b_child && one.i_ppid == getpid()
                     && ksft_streq(one.arr_comm, me.arr_comm),
                     "a child waiting on a pipe sleeps, under this one's "
                     "name, and says whose it is\n");

    int i_status = 0;
    bool b_stopped = i_child > 0 && kill(i_child, SIGSTOP) == 0
                     && waitpid(i_child, &i_status, WUNTRACED) == i_child
                     && WIFSTOPPED(i_status);
    ksft_test_result(b_stopped && state_becomes(i_child, 'T') == 'T',
                     "stopped, it says so\n");

    bool b_killed = i_child > 0 && kill(i_child, SIGKILL) == 0;
    char c_dead = b_killed ? state_becomes(i_child, 'Z') : '?';
    bool b_reaped = b_killed && waitpid(i_child, &i_status, 0) == i_child
                    && ksft_killed_by(i_status, SIGKILL);
    close(arr_pipe[0]);
    close(arr_pipe[1]);
    ksft_test_result(c_dead == 'Z' && b_reaped && !stat_of(i_child, &one),
                     "killed and not waited for it is a zombie, and waited "
                     "for, it is gone\n");

    check_threads();
    ksft_finished();
}

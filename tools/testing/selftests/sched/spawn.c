// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/spawn.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * posix_spawn, and a child running in its parent's memory until it execs.
 */

#define _GNU_SOURCE
#include "../kselftest.h"

#include <sched.h>
#include <spawn.h>

extern char **environ;

#define STACK_BYTES 65536

static char s_arr_stack[STACK_BYTES] __attribute__((aligned(16)));
static volatile int s_i_seen;

/* a child in its parent's memory, with its parent waiting for it */
static int write_and_go(void *p_arg)
{
    (void)p_arg;
    struct timespec ts = { 0, 30000000 };
    nanosleep(&ts, NULL);
    s_i_seen = 0x77;
    _exit(3);
}

/* and one with its parent running beside it */
static int write_and_wait(void *p_arg)
{
    (void)p_arg;
    s_i_seen = 0x99;
    while (s_i_seen != 0x100) {
        sched_yield();
    }
    _exit(4);
}

static int status_of(pid_t i_pid)
{
    int i_status = 0;
    return (waitpid(i_pid, &i_status, 0) == i_pid) ? i_status : -1;
}

int main(int i_argc, char **arr_argv)
{
    /* what the spawned copy of this program does: end, saying so if asked */
    if (i_argc == 3 && (ksft_streq(arr_argv[1], "spawned")
                        || ksft_streq(arr_argv[1], "say"))) {
        if (ksft_streq(arr_argv[1], "say")) {
            write(1, "said", 4);
        }
        return atoi(arr_argv[2]);
    }

    ksft_print_header();
    ksft_set_plan(5);

    pid_t i_kid = 0;
    char *arr_args[] = { arr_argv[0], "spawned", "21", NULL };
    int i_why = posix_spawn(&i_kid, arr_argv[0], NULL, NULL, arr_args,
                            environ);
    ksft_test_result(i_why == 0 && ksft_exited(status_of(i_kid), 21),
                     "posix_spawn runs a program, and wait says how it "
                     "ended\n");

    char *arr_none[] = { "nothing", NULL };
    i_why = posix_spawn(&i_kid, "/no/such/program", NULL, NULL, arr_none,
                        environ);
    ksft_test_result(i_why == ENOENT,
                     "and a program that is not there is said not to be\n");

    int arr_pipe[2];
    pipe(arr_pipe);
    posix_spawn_file_actions_t acts;
    posix_spawn_file_actions_init(&acts);
    posix_spawn_file_actions_adddup2(&acts, arr_pipe[1], 1);
    posix_spawn_file_actions_addclose(&acts, arr_pipe[0]);
    char *arr_say[] = { arr_argv[0], "say", "21", NULL };
    i_why = posix_spawn(&i_kid, arr_argv[0], &acts, NULL, arr_say, environ);
    close(arr_pipe[1]);
    char arr_said[8] = { 0 };
    bool b_said = read(arr_pipe[0], arr_said, 4) == 4
                  && memcmp(arr_said, "said", 4) == 0;
    ksft_test_result(i_why == 0 && b_said
                     && ksft_exited(status_of(i_kid), 21),
                     "with its descriptors arranged first\n");
    posix_spawn_file_actions_destroy(&acts);

    /*
     * the child posix_spawn makes runs in its parent's memory, the
     * parent waiting: a write it makes is the parent's
     */
    s_i_seen = 0;
    i_kid = clone(write_and_go, s_arr_stack + STACK_BYTES,
                  CLONE_VM | CLONE_VFORK | SIGCHLD, NULL);
    bool b_first = s_i_seen == 0x77;
    ksft_test_result(i_kid > 0 && b_first && ksft_exited(status_of(i_kid), 3),
                     "a child in its parent's memory writes the parent's, "
                     "and the parent waits for it to end\n");

    s_i_seen = 0;
    i_kid = clone(write_and_wait, s_arr_stack + STACK_BYTES,
                  CLONE_VM | SIGCHLD, NULL);
    while (i_kid > 0 && s_i_seen != 0x99) {
        sched_yield();
    }
    s_i_seen = 0x100;
    ksft_test_result(i_kid > 0 && ksft_exited(status_of(i_kid), 4),
                     "or runs beside it, each seeing what the other "
                     "writes\n");
    ksft_finished();
}

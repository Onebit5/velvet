// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/exec.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * exec: a process becoming another program, and what it keeps.
 *
 * the program it becomes is this one, started with "again". that image
 * checks what it was handed and answers in its exit code, since the TAP
 * count lives in the memory exec throws away
 */

#include "../kselftest.h"

#define AGAIN       "again"
#define EXEC_FAILED 9
#define COPY        "/tmp/exec-copy"

/* this program copied to str_to, runnable: whether it all went */
static bool copy_self(const char *str_from, const char *str_to)
{
    static char s_arr_bytes[1 << 20];
    int i_in = open(str_from, O_RDONLY);
    int64_t i_len = (i_in >= 0) ? read(i_in, s_arr_bytes, sizeof s_arr_bytes)
                                : -1;
    int i_out = open(str_to, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    bool b_ok = i_len > 0 && (size_t)i_len < sizeof s_arr_bytes && i_out >= 0
                && write(i_out, s_arr_bytes, (size_t)i_len) == i_len;
    close(i_in);
    close(i_out);
    return b_ok;
}

/* 0 when all is as it should be, else which part was not */
static int again(int i_argc, char **pp_argv)
{
    char arr_text[16];

    if (i_argc != 5) {
        return 1;
    }
    if (atoi(pp_argv[2]) != getpid()) {
        return 2;
    }
    if (read(atoi(pp_argv[3]), arr_text, 15) <= 0
        || memcmp(arr_text, "velvet", 6) != 0) {
        return 3;
    }
    if (fcntl(atoi(pp_argv[4]), F_GETFD) != -1 || errno != EBADF) {
        return 4;
    }
    return 0;
}

int main(int i_argc, char **pp_argv)
{
    char arr_pid[12];
    char arr_fd[12];
    char arr_gone[12];
    int i_status = -1;

    if (i_argc >= 2 && ksft_streq(pp_argv[1], AGAIN)) {
        return again(i_argc, pp_argv);
    }

    ksft_print_header();
    ksft_set_plan(8);

    int i_fd = open("/proc/version", O_RDONLY);
    int i_closing = open("/proc/version", O_RDONLY | O_CLOEXEC);
    pid_t i_pid = fork();
    if (i_pid == 0) {
        char *arr_args[] = { pp_argv[0], AGAIN, arr_pid, arr_fd, arr_gone,
                             NULL };

        snprintf(arr_pid, sizeof arr_pid, "%d", getpid());
        snprintf(arr_fd, sizeof arr_fd, "%d", i_fd);
        snprintf(arr_gone, sizeof arr_gone, "%d", i_closing);
        execv(pp_argv[0], arr_args);
        _exit(EXEC_FAILED);
    }
    waitpid(i_pid, &i_status, 0);
    int i_code = WIFEXITED(i_status) ? WEXITSTATUS(i_status) : -1;

    ksft_test_result(i_code != EXEC_FAILED && i_code != 1,
                     "exec starts the program, with its arguments\n");
    ksft_test_result(i_code == 0 || i_code >= 3,
                     "in the same process, under the same pid\n");
    ksft_test_result(i_code == 0 || i_code == 4,
                     "holding the descriptors it held\n");
    ksft_test_result(i_code == 0,
                     "except those marked to close on exec\n");

    /*
     * the same program from a file of its own on the disk, whose pages
     * are read off it as the program touches them, with nothing of it
     * read before it starts
     */
    bool b_copied = copy_self(pp_argv[0], COPY);
    lseek(i_fd, 0, SEEK_SET);
    i_status = -1;
    i_pid = b_copied ? fork() : -1;
    if (i_pid == 0) {
        char *arr_args[] = { COPY, AGAIN, arr_pid, arr_fd, arr_gone, NULL };
        snprintf(arr_pid, sizeof arr_pid, "%d", getpid());
        snprintf(arr_fd, sizeof arr_fd, "%d", i_fd);
        snprintf(arr_gone, sizeof arr_gone, "%d", i_closing);
        execv(COPY, arr_args);
        _exit(EXEC_FAILED);
    }
    if (i_pid > 0) {
        waitpid(i_pid, &i_status, 0);
    }
    unlink(COPY);
    ksft_test_result(b_copied && WIFEXITED(i_status)
                     && WEXITSTATUS(i_status) == 0,
                     "a program on the disk runs from there\n");

    char *arr_none[] = { "nothing", NULL };
    ksft_test_result(execv("/nothing/here", arr_none) == -1
                     && errno == ENOENT,
                     "a path with nothing there is ENOENT, and the caller "
                     "carries on\n");
    ksft_test_result(execv("/", arr_none) == -1 && errno == EACCES,
                     "a directory is not a program\n");
    ksft_test_result(execv(pp_argv[0], (char *const *)0xffff800000000000ull)
                     == -1 && errno == EFAULT,
                     "an argv in the kernel's memory is refused\n");
    ksft_finished();
}

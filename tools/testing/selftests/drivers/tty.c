// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/drivers/tty.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the terminal: who is in front of it, and who may say so.
 *
 * a process that is not in front has what it prints dropped, so the
 * checks made while the child holds the terminal are kept and printed
 * once the child has handed it back
 */

#include "../kselftest.h"

#include <sys/ioctl.h>
#include <termios.h>

/*
 * a child in a group of its own, which does `what` once it is behind the
 * test's group, and is stopped for it
 */
static pid_t behind(int i_what)
{
    pid_t i_pid = fork();

    if (i_pid == 0) {
        char c_ch;
        setpgid(0, 0);
        if (i_what == 0) {
            read(0, &c_ch, 1);
        } else {
            tcsetpgrp(0, getpgrp());
        }
        _exit(0);
    }
    setpgid(i_pid, i_pid);
    return i_pid;
}

/* whether a child behind is stopped by `i_sig`, ended afterwards either way */
static bool stopped_by(pid_t i_pid, int i_sig)
{
    int i_status = 0;
    bool b_stopped = waitpid(i_pid, &i_status, WUNTRACED) == i_pid
                     && WIFSTOPPED(i_status) && WSTOPSIG(i_status) == i_sig;
    kill(i_pid, SIGKILL);
    waitpid(i_pid, &i_status, 0);
    return b_stopped;
}

int main(void)
{
    pid_t i_me = getpid();
    int i_status = -1;
    struct winsize size;

    ksft_print_header();
    ksft_set_plan(9);

    /* run as a job of one, so its group is its pid */
    ksft_test_result(tcgetpgrp(0) == i_me,
                     "the job being run is in front of the terminal\n");
    ksft_test_result(ioctl(0, TIOCSCTTY, 0) == -1 && errno == EPERM,
                     "a process that leads no session may not take one\n");
    ksft_test_result(tcsetpgrp(0, 30000) == -1 && errno == EPERM,
                     "nor hand it to a group that is not there\n");

    pid_t i_pid = fork();
    if (i_pid == 0) {
        ksft_sleep_ms(300);
        _exit(tcsetpgrp(0, i_me) == 0 ? 0 : 1);
    }
    setpgid(i_pid, i_pid);
    signal(SIGTTOU, SIG_IGN);
    bool b_given = tcsetpgrp(0, i_pid) == 0 && tcgetpgrp(0) == i_pid;
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(b_given,
                     "the group in front may give the terminal away\n");
    ksft_test_result(ksft_exited(i_status, 0) && tcgetpgrp(0) == i_me,
                     "and the group it went to may hand it back\n");

    /*
     * a group behind, ignoring SIGTTOU as a shell does, may take it, and
     * that is how a shell takes it back from a job that stopped
     */
    i_pid = fork();
    if (i_pid == 0) {
        setpgid(0, 0);
        signal(SIGTTOU, SIG_IGN);
        _exit(tcsetpgrp(0, getpgrp()) == 0 ? 0 : 1);
    }
    setpgid(i_pid, i_pid);
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, 0) && tcgetpgrp(0) == i_pid,
                     "a group behind that ignores SIGTTOU may take it\n");
    tcsetpgrp(0, i_me);
    signal(SIGTTOU, SIG_DFL);

    ksft_test_result(stopped_by(behind(1), SIGTTOU),
                     "one that does not is stopped by SIGTTOU instead\n");
    ksft_test_result(stopped_by(behind(0), SIGTTIN),
                     "and a read from behind is stopped by SIGTTIN\n");

    /* the size is written where it is asked for, so that has to be writable */
    static const struct winsize s_ro;
    ksft_test_result(ioctl(0, TIOCGWINSZ, &size) == 0 && size.ws_row > 0
                     && size.ws_col > 0
                     && ioctl(0, TIOCGWINSZ, (void *)&s_ro) == -1
                     && errno == EFAULT,
                     "the size is told, and not into what is read only\n");
    ksft_finished();
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/drivers/keyboard.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program waiting for a key, and nobody typing: an interrupt has to
 * end the wait anyway, or a reader can only ever be woken by a key.
 */

#include "../kselftest.h"

#include <termios.h>

static void caught(int i_sig)
{
    (void)i_sig;
}

/* a child that interrupts its parent once the parent has had time to wait */
static pid_t interrupter(pid_t i_parent)
{
    pid_t i_pid = fork();

    if (i_pid == 0) {
        ksft_sleep_ms(300);
        kill(i_parent, SIGINT);
        _exit(0);
    }
    return i_pid;
}

int main(void)
{
    char arr_line[16];
    int i_status;
    struct termios was;
    struct termios raw;

    ksft_print_header();
    ksft_set_plan(3);

    /*
     * caught without SA_RESTART, so the wait ends rather than starting
     * again, and the test is left alive
     */
    struct sigaction act = { .sa_handler = caught };
    sigaction(SIGINT, &act, NULL);

    pid_t i_pid = interrupter(getpid());
    ksft_test_result(read(0, arr_line, sizeof arr_line) == -1
                     && errno == EINTR,
                     "a signal ends a read of a line from the keyboard\n");
    waitpid(i_pid, &i_status, 0);

    tcgetattr(0, &was);
    raw = was;
    cfmakeraw(&raw);
    ksft_test_result(tcsetattr(0, TCSANOW, &raw) == 0,
                     "the terminal can be made raw\n");
    i_pid = interrupter(getpid());
    ksft_test_result(read(0, arr_line, 1) == -1 && errno == EINTR,
                     "and a wait for one key ends the same way\n");
    waitpid(i_pid, &i_status, 0);
    tcsetattr(0, TCSANOW, &was);
    ksft_finished();
}

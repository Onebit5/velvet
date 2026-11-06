// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/fifo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * fifos: a pipe with a name, which two programs that share nothing but
 * the name open from either end.
 */

#include "../kselftest.h"

#include <poll.h>
#include <sys/stat.h>

#define AT_FIFO  "/tmp/fifo-test"
#define TEXT_MAX 32

/* a child that opens the fifo for writing, says str_text and goes */
static pid_t writer(const char *str_text)
{
    pid_t i_pid = fork();
    if (i_pid == 0) {
        int i_fd = open(AT_FIFO, O_WRONLY);
        write(i_fd, str_text, strlen(str_text));
        _exit(i_fd >= 0 ? 0 : 1);
    }
    return i_pid;
}

int main(void)
{
    char arr_text[TEXT_MAX];
    struct stat st;
    int i_status = -1;

    ksft_print_header();
    ksft_set_plan(8);

    unlink(AT_FIFO);
    ksft_test_result(mkfifo(AT_FIFO, 0600) == 0 && stat(AT_FIFO, &st) == 0
                     && S_ISFIFO(st.st_mode) && st.st_uid == getuid(),
                     "anyone may make a fifo, and it is one\n");
    ksft_test_result(open(AT_FIFO, O_WRONLY | O_NONBLOCK) < 0
                     && errno == ENXIO,
                     "a writer that may not wait for a reader is ENXIO\n");
    int i_quick = open(AT_FIFO, O_RDONLY | O_NONBLOCK);
    ksft_test_result(i_quick >= 0 && read(i_quick, arr_text, TEXT_MAX) == 0,
                     "and a reader that may not wait opens at once, and "
                     "reads the end with nobody writing\n");
    close(i_quick);

    /* each end waits in open for the other, and then they are a pipe */
    pid_t i_pid = writer("a name");
    int i_fd = open(AT_FIFO, O_RDONLY);
    ssize_t i_got = (i_fd >= 0) ? read(i_fd, arr_text, TEXT_MAX) : -1;
    ksft_test_result(i_got == 6 && memcmp(arr_text, "a name", 6) == 0,
                     "a reader's open waits for a writer, and reads what "
                     "it writes\n");
    ksft_test_result(read(i_fd, arr_text, TEXT_MAX) == 0
                     && waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_exited(i_status, 0),
                     "and the end once the writer is gone\n");
    ksft_test_result(fstat(i_fd, &st) == 0 && S_ISFIFO(st.st_mode)
                     && lseek(i_fd, 0, SEEK_SET) < 0 && errno == ESPIPE,
                     "an open fifo is the fifo, and has no position\n");
    close(i_fd);

    /* a writer waits too, and is told EPIPE once the reader goes */
    i_pid = fork();
    if (i_pid == 0) {
        int i_end = open(AT_FIFO, O_RDONLY);
        read(i_end, arr_text, 1);
        _exit(0);
    }
    signal(SIGPIPE, SIG_IGN);
    i_fd = open(AT_FIFO, O_WRONLY);
    write(i_fd, "x", 1);
    waitpid(i_pid, &i_status, 0);
    ksft_test_result(i_fd >= 0 && write(i_fd, "y", 1) < 0 && errno == EPIPE,
                     "a writer's open waits for a reader, and writing "
                     "once it has gone is EPIPE\n");
    close(i_fd);

    int i_both = open(AT_FIFO, O_RDWR);
    ksft_test_result(i_both >= 0 && write(i_both, "z", 1) == 1
                     && read(i_both, arr_text, 1) == 1 && arr_text[0] == 'z'
                     && close(i_both) == 0 && unlink(AT_FIFO) == 0,
                     "one opened for both ends waits for nobody, and the "
                     "name goes like any other\n");
    ksft_finished();
}

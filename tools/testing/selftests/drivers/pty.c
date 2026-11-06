// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/drivers/pty.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the pty: a terminal with this program at both ends of it, the way a
 * terminal emulator, script and a multiplexer are.
 */

#include "../kselftest.h"

#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <termios.h>

#define TEXT_MAX 64
#define WAIT_MS  2000

static volatile sig_atomic_t s_i_winches;
static volatile sig_atomic_t s_i_hups;

static void on_winch(int i_sig)
{
    (void)i_sig;
    s_i_winches++;
}

static void on_hup(int i_sig)
{
    (void)i_sig;
    s_i_hups++;
}

/* where a child says what happened to it */
static int s_i_said = -1;

static void on_hup_say(int i_sig)
{
    (void)i_sig;
    write(s_i_said, "H", 1);
    _exit(0);
}

/* the first byte said on i_fd within WAIT_MS, or 0 */
static char heard(int i_fd)
{
    char c_ch = 0;
    struct pollfd fd = { .fd = i_fd, .events = POLLIN };
    if (poll(&fd, 1, WAIT_MS) == 1 && read(i_fd, &c_ch, 1) != 1) {
        c_ch = 0;
    }
    return c_ch;
}

/*
 * a session's leader on the slave named str_name, with a job in front
 * that hears what becomes of it on arr_said, and the leader ending once
 * the job is ready
 */
static void leader_leaves(const char *str_name, int i_master, int *arr_said)
{
    int arr_ready[2];
    pipe(arr_ready);
    if (fork() != 0) {
        close(arr_ready[0]);
        close(arr_ready[1]);
        return;
    }
    close(i_master);
    setsid();
    open(str_name, O_RDWR);
    if (fork() == 0) {
        s_i_said = arr_said[1];
        signal(SIGHUP, on_hup_say);
        write(arr_ready[1], "R", 1);
        for (;;) {
            pause();
        }
    }
    heard(arr_ready[0]);
    _exit(0);
}

/*
 * a job behind the slave's group in front whose parent leaves, which
 * orphans it, the session going on without it: what a read of the
 * terminal tells it, on arr_said, E for -EIO. the session's leader
 * stays until the master closes
 */
static void orphan_reads(const char *str_name, int i_master, int *arr_said)
{
    if (fork() != 0) {
        return;
    }
    close(i_master);
    setsid();
    int i_fd = open(str_name, O_RDWR);
    if (fork() == 0) {
        if (fork() == 0) {
            setpgid(0, 0);
            while (getppid() != 1) {
                ksft_sleep_ms(10);
            }
            char c_ch;
            bool b_eio = read(i_fd, &c_ch, 1) < 0 && errno == EIO;
            write(arr_said[1], b_eio ? "E" : "?", 1);
            _exit(0);
        }
        _exit(0);
    }
    char c_ch;
    read(i_fd, &c_ch, 1);
    _exit(0);
}

/* what i_fd has to read within WAIT_MS, as a string: how much, 0 for none */
static ssize_t take(int i_fd, char *str_buf)
{
    struct pollfd fd = { .fd = i_fd, .events = POLLIN };
    if (poll(&fd, 1, WAIT_MS) <= 0 || !(fd.revents & POLLIN)) {
        str_buf[0] = '\0';
        return 0;
    }
    ssize_t i_got = read(i_fd, str_buf, TEXT_MAX - 1);
    str_buf[i_got > 0 ? i_got : 0] = '\0';
    return i_got;
}

/* the slave's name, into str_out: true, or false with none */
static bool name_of(int i_master, char *str_out)
{
    const char *str_name = ptsname(i_master);
    if (str_name == NULL) {
        return false;
    }
    snprintf(str_out, TEXT_MAX, "%s", str_name);
    return true;
}

/* whether nothing turns up at i_fd for a while */
static bool quiet(int i_fd)
{
    struct pollfd fd = { .fd = i_fd, .events = POLLIN };
    return poll(&fd, 1, 200) == 0;
}

/*
 * a child that leads a session of its own on the slave named str_name,
 * which says R on it once it is ready and then does i_what: 0 waits on
 * a read for whatever comes, saying W on each SIGWINCH; 1 waits for the
 * terminal to be hung up, and says how it went in its exit code. it lets
 * go of the master it was born holding, i_master, as a program on a
 * pty does, or the master would outlive the parent's closing it
 */
static pid_t session_on(const char *str_name, int i_master, int i_what)
{
    pid_t i_pid = fork();
    if (i_pid != 0) {
        return i_pid;
    }
    close(i_master);
    setsid();
    int i_fd = open(str_name, O_RDWR | O_NOCTTY);
    if (i_fd < 0 || ioctl(i_fd, TIOCSCTTY, 0) != 0) {
        _exit(2);
    }
    /* a resize ends the read rather than having it made again */
    struct sigaction winch = { .sa_handler = on_winch };
    sigaction(SIGWINCH, &winch, NULL);
    signal(SIGHUP, on_hup);
    write(i_fd, "R", 1);
    char c_ch;
    if (i_what == 0) {
        for (;;) {
            int i_seen = s_i_winches;
            if (read(i_fd, &c_ch, 1) < 0 && errno == EINTR
                && s_i_winches != i_seen) {
                write(i_fd, "W", 1);
            }
        }
    }
    ssize_t i_got = read(i_fd, &c_ch, 1);
    bool b_eio = write(i_fd, "z", 1) < 0 && errno == EIO;
    _exit((i_got == 0 && b_eio && s_i_hups == 1) ? 0 : 1);
}

int main(void)
{
    char arr_text[TEXT_MAX];
    char arr_name[TEXT_MAX];
    struct stat st;
    int i_status = -1;

    ksft_print_header();
    ksft_set_plan(19);

    int i_m = posix_openpt(O_RDWR | O_NOCTTY);
    bool b_named = i_m >= 0 && grantpt(i_m) == 0
                   && name_of(i_m, arr_name)
                   && strncmp(arr_name, "/dev/pts/", 9) == 0;
    ksft_test_result(b_named, "posix_openpt makes a pty, and ptsname names "
                     "its slave %s\n", b_named ? arr_name : "nothing");
    ksft_test_result(stat(arr_name, &st) == 0 && S_ISCHR(st.st_mode)
                     && major(st.st_rdev) == 136 && st.st_uid == getuid(),
                     "which is a terminal's device, 136 and N, and ours\n");
    ksft_test_result(open(arr_name, O_RDWR | O_NOCTTY) < 0 && errno == EIO,
                     "it may not be opened until unlockpt\n");
    int i_s = (unlockpt(i_m) == 0) ? open(arr_name, O_RDWR | O_NOCTTY) : -1;
    ksft_test_result(i_s >= 0 && isatty(i_s) && isatty(i_m),
                     "and then it may, and both ends are terminals\n");

    write(i_m, "hello\r", 6);
    ksft_test_result(take(i_s, arr_text) == 6
                     && ksft_streq(arr_text, "hello\n"),
                     "what the master writes is a line typed at the slave\n");
    ksft_test_result(take(i_m, arr_text) == 7
                     && ksft_streq(arr_text, "hello\r\n"),
                     "and its echo comes back to the master\n");
    write(i_s, "a\nb", 3);
    ksft_test_result(take(i_m, arr_text) == 4
                     && ksft_streq(arr_text, "a\r\nb"),
                     "what the slave writes, the master reads, newlines "
                     "made two\n");

    struct termios mode, raw;
    tcgetattr(i_s, &mode);
    raw = mode;
    cfmakeraw(&raw);
    tcsetattr(i_m, TCSANOW, &raw);
    tcgetattr(i_s, &raw);
    write(i_m, "x", 1);
    ksft_test_result(!(raw.c_lflag & ICANON) && take(i_s, arr_text) == 1
                     && arr_text[0] == 'x' && quiet(i_m),
                     "modes set on the master are the slave's: raw, a "
                     "byte at a time, and no echo\n");
    tcsetattr(i_s, TCSANOW, &mode);

    /* a session on the slave: its size, who is in front, and ctrl+c */
    pid_t i_pid = session_on(arr_name, i_m, 0);
    bool b_ready = take(i_m, arr_text) == 1 && arr_text[0] == 'R';
    pid_t i_front = 0;
    ksft_test_result(b_ready && ioctl(i_m, TIOCGPGRP, &i_front) == 0
                     && i_front == i_pid,
                     "a session leader takes the slave, and the master "
                     "is told its group is in front\n");
    struct winsize size = { .ws_row = 50, .ws_col = 132 }, told = { 0 };
    ioctl(i_m, TIOCSWINSZ, &size);
    ksft_test_result(take(i_m, arr_text) == 1 && arr_text[0] == 'W'
                     && ioctl(i_s, TIOCGWINSZ, &told) == 0
                     && told.ws_row == 50 && told.ws_col == 132,
                     "a size the master sets is SIGWINCH to it, and the "
                     "size the slave is told\n");
    write(i_m, "\x03", 1);
    ksft_test_result(waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_killed_by(i_status, SIGINT)
                     && take(i_m, arr_text) == 2
                     && ksft_streq(arr_text, "^C"),
                     "and ctrl+c written to the master is SIGINT to it, "
                     "echoed as ^C\n");

    /* a session leader with no terminal takes the one it opens */
    pid_t i_kid = fork();
    if (i_kid == 0) {
        close(i_m);
        int i_quiet = open(arr_name, O_RDWR | O_NOCTTY);
        bool b_not_leader = tcgetsid(open(arr_name, O_RDWR)) < 0;
        setsid();
        bool b_noctty = tcgetsid(i_quiet) < 0
                        && tcgetsid(open(arr_name, O_RDWR | O_NOCTTY)) < 0
                        && tcgetsid(open(arr_name, O_WRONLY)) < 0;
        int i_own = open(arr_name, O_RDWR);
        bool b_taken = tcgetsid(i_own) == getpid()
                       && tcgetpgrp(i_own) == getpgrp();
        _exit((b_not_leader ? 0 : 1) | (b_noctty ? 0 : 2)
              | (b_taken ? 0 : 4));
    }
    waitpid(i_kid, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, 0),
                     "a session leader with no terminal takes the slave by "
                     "opening it, unless it says O_NOCTTY or only writes, "
                     "and nobody else does\n");

    /* a session's leader ends, and the group in front of it hears */
    int arr_said[2];
    pipe(arr_said);
    leader_leaves(arr_name, i_m, arr_said);
    ksft_test_result(heard(arr_said[0]) == 'H',
                     "a session's leader ending is SIGHUP to the group in "
                     "front of its terminal\n");
    orphan_reads(arr_name, i_m, arr_said);
    ksft_test_result(heard(arr_said[0]) == 'E',
                     "and a job left orphaned behind a terminal reads EIO "
                     "rather than being stopped\n");
    /* the leader reads a line and goes, and its echo is read off here */
    write(i_m, "\n", 1);
    take(i_m, arr_text);
    close(arr_said[0]);
    close(arr_said[1]);
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }

    /* the slave closed, and then the master */
    close(i_s);
    struct pollfd fd = { .fd = i_m, .events = POLLIN };
    ksft_test_result(read(i_m, arr_text, TEXT_MAX) < 0 && errno == EIO
                     && poll(&fd, 1, 0) == 1 && (fd.revents & POLLHUP),
                     "with the slave closed the master reads EIO\n");
    close(i_m);
    ksft_test_result(stat(arr_name, &st) < 0 && errno == ENOENT,
                     "and with the master closed %s is gone\n", arr_name);

    int i_peer = -1;
    char arr_other[TEXT_MAX];
    ksft_test_result(openpty(&i_m, &i_s, arr_other, NULL, NULL) == 0
                     && (i_peer = ioctl(i_m, TIOCGPTPEER,
                                        O_RDWR | O_NOCTTY)) >= 0
                     && write(i_peer, "p", 1) == 1
                     && take(i_m, arr_text) == 1 && arr_text[0] == 'p',
                     "openpty makes one too, and TIOCGPTPEER opens its "
                     "slave from the master\n");
    close(i_peer);

    /* the slave stays open until the child has it, or the master hears it go */
    i_pid = session_on(arr_other, i_m, 1);
    b_ready = take(i_m, arr_text) == 1 && arr_text[0] == 'R';
    close(i_s);
    close(i_m);
    i_status = -1;
    ksft_test_result(b_ready && waitpid(i_pid, &i_status, 0) == i_pid
                     && ksft_exited(i_status, 0),
                     "closing the master is SIGHUP to the slave's session "
                     "leader, whose terminal reads the end and writes EIO\n");

    i_m = posix_openpt(O_RDWR | O_NOCTTY);
    ksft_test_result(i_m >= 0 && name_of(i_m, arr_other)
                     && ksft_streq(arr_other, arr_name),
                     "and the pty's number is free for the next\n");
    close(i_m);
    ksft_finished();
}

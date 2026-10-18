// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/init.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the init of the kernel's own image: asks which selftests to run, runs
 * each one as a guest, the way a system runs a program, and powers the
 * machine off. the kernel hands the machine to /init, and this is the
 * smallest /init that checks anything.
 */

#include "kselftest.h"

#define GUEST_UID    1000       /* the account the kernel does least for */
#define ANSWER_MAX   2048
#define TESTS_MAX    128
#define NAME_MAX     64
#define TIMEOUT_S    30
#define POLL_MS      20
#define TIMED_OUT    (-1)

/* what ends a program, which pid 1 ignores and a test is given back */
static const int s_arr_signals[] = { SIGINT, SIGTSTP, SIGTERM, SIGHUP };

static char s_arr_answer[ANSWER_MAX];
static char s_arr_found[TESTS_MAX][NAME_MAX];
static const char *s_arr_tests[TESTS_MAX];
static int s_i_tests;
static int64_t s_i_timeout_s = TIMEOUT_S;

static __attribute__((format(printf, 1, 2))) void say(const char *str_fmt, ...)
{
    va_list args;

    va_start(args, str_fmt);
    ksft_print("init: ", str_fmt, args);
    va_end(args);
}

static void set_signals(uint64_t u_handler)
{
    size_t sz_count = sizeof s_arr_signals / sizeof s_arr_signals[0];

    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        ksft_syscall(SYS_SIGNAL, s_arr_signals[sz_at], (int64_t)u_handler, 0);
    }
}

static int64_t uptime_ms(void)
{
    return ksft_syscall(SYS_UPTIME, 0, 0, 0);
}

/* `/selftests/` and the name, which is where a test is on the medium */
static bool path_of(const char *str_name, char *str_path, size_t sz_size)
{
    static const char s_arr_dir[] = "/selftests/";
    size_t sz_dir = sizeof s_arr_dir - 1;
    size_t sz_name = ksft_strlen(str_name);

    if (sz_dir + sz_name + 1 > sz_size) {
        return false;
    }
    for (size_t sz_at = 0; sz_at < sz_dir; sz_at++) {
        str_path[sz_at] = s_arr_dir[sz_at];
    }
    for (size_t sz_at = 0; sz_at <= sz_name; sz_at++) {
        str_path[sz_dir + sz_at] = str_name[sz_at];
    }
    return true;
}

/* every test on the medium, subsystem by subsystem, as `subsystem/test` */
static void find_all(void)
{
    char arr_subsystem[NAME_MAX];
    char arr_test[NAME_MAX];
    char arr_dir[NAME_MAX * 2];

    s_i_tests = 0;
    for (size_t sz_s = 0; readdir("/selftests", sz_s, arr_subsystem,
                                  sizeof arr_subsystem) > 0; sz_s++) {
        if (!path_of(arr_subsystem, arr_dir, sizeof arr_dir)) {
            continue;
        }
        for (size_t sz_t = 0; readdir(arr_dir, sz_t, arr_test,
                                      sizeof arr_test) > 0; sz_t++) {
            if (s_i_tests == TESTS_MAX) {
                return;
            }
            char *str_out = s_arr_found[s_i_tests];
            size_t sz_at = 0;
            /* the subsystem's name already ends in its / */
            for (const char *p = arr_subsystem; *p && sz_at < NAME_MAX - 1;) {
                str_out[sz_at++] = *p++;
            }
            for (const char *p = arr_test; *p && sz_at < NAME_MAX - 1;) {
                str_out[sz_at++] = *p++;
            }
            str_out[sz_at] = '\0';
            s_arr_tests[s_i_tests++] = str_out;
        }
    }
}

static bool starts_with(const char *str_text, const char *str_prefix)
{
    size_t sz_len = ksft_strlen(str_prefix);

    return ksft_strlen(str_text) >= sz_len
           && ksft_memeq(str_text, str_prefix, sz_len);
}

/*
 * the answer, split into words where it is read: `all`, `none`,
 * `timeout=SECONDS` or a test's name. true with s_arr_tests filled in,
 * or false when it has to be asked again
 */
static bool understand(bool *p_none)
{
    bool b_all = false;
    char *str_at = s_arr_answer;

    *p_none = false;
    s_i_tests = 0;
    for (;;) {
        while (*str_at == ' ' || *str_at == '\t' || *str_at == '\n') {
            *str_at++ = '\0';
        }
        if (*str_at == '\0') {
            break;
        }
        char *str_word = str_at;
        while (*str_at != '\0' && *str_at != ' ' && *str_at != '\t'
               && *str_at != '\n') {
            str_at++;
        }
        if (*str_at != '\0') {
            *str_at++ = '\0';
        }

        if (ksft_streq(str_word, "all")) {
            b_all = true;
        } else if (ksft_streq(str_word, "none")) {
            *p_none = true;
        } else if (starts_with(str_word, "timeout=")) {
            const char *p_digit = str_word + 8;
            int64_t i_seconds = 0;
            while (*p_digit >= '0' && *p_digit <= '9' && i_seconds < 100000) {
                i_seconds = i_seconds * 10 + (*p_digit++ - '0');
            }
            if (*p_digit != '\0' || i_seconds <= 0) {
                say("%s is not a number of seconds\n", str_word);
                return false;
            }
            s_i_timeout_s = i_seconds;
        } else {
            char arr_path[NAME_MAX * 2];
            int i_fd = path_of(str_word, arr_path, sizeof arr_path)
                       ? open(arr_path) : -1;
            if (i_fd < 0) {
                say("there is no selftests/%s\n", str_word);
                return false;
            }
            close(i_fd);
            if (s_i_tests == TESTS_MAX) {
                say("no more than %d tests at a time\n", TESTS_MAX);
                return false;
            }
            s_arr_tests[s_i_tests++] = str_word;
        }
    }

    if (b_all) {
        find_all();
    }
    if (!*p_none && s_i_tests == 0) {
        say("that names no selftests\n");
        return false;
    }
    return true;
}

/*
 * the child: in a group of its own, with the signals pid 1 ignores back
 * to what a program expects, and the console before anything else, since
 * what a group not in front writes goes nowhere. a guest only after
 * that, because handing the console over is root's
 */
static __attribute__((noreturn)) void become(const char *str_path)
{
    char *arr_argv[] = { (char *)str_path, NULL };

    setpgid(0, 0);
    set_signals(SIG_DEFAULT);
    while (tcgetpgrp() != getpid()) {
        sleep_ms(1);
    }
    if (ksft_syscall(SYS_SETUID, GUEST_UID, 0, 0) != 0) {
        exit(126);
    }
    execv(str_path, arr_argv);
    exit(127);
}

/*
 * one test, from its start to how it ended: what it passed to exit, 128
 * plus the signal that ended it, or TIMED_OUT when it had to be killed
 */
static int run(const char *str_name)
{
    char arr_path[NAME_MAX * 2];
    int i_status = 0;

    path_of(str_name, arr_path, sizeof arr_path);
    say("selftests/%s starts\n", str_name);

    int i_pid = fork();
    if (i_pid == 0) {
        become(arr_path);
    }
    if (i_pid < 0) {
        return 127;
    }
    setpgid(i_pid, i_pid);
    tcsetpgrp(i_pid);

    int64_t i_until = uptime_ms() + s_i_timeout_s * 1000;
    for (;;) {
        int i_got = waitpid(i_pid, &i_status, WNOHANG);
        if (i_got == i_pid || i_got < 0) {
            break;
        }
        if (uptime_ms() >= i_until) {
            sigsend(-i_pid, SIGKILL);
            waitpid(i_pid, &i_status, 0);
            i_status = TIMED_OUT;
            break;
        }
        sleep_ms(POLL_MS);
    }

    /* the console back, and whatever the test left behind collected */
    tcsetpgrp(getpid());
    while (waitpid(-1, (int *)0, WNOHANG) > 0) {
    }
    return i_status;
}

/* pid 1 has nothing left to do but collect orphans */
static __attribute__((noreturn)) void idle(void)
{
    for (;;) {
        if (waitpid(-1, (int *)0, 0) < 0) {
            sleep_ms(1000);
        }
    }
}

int main(int i_argc, char **pp_argv)
{
    bool b_none = false;
    int i_passed = 0;

    (void)i_argc;
    (void)pp_argv;

    /* pid 1 ending is the end of the machine, so these are not for it */
    set_signals(SIG_IGNORE);
    ksft_syscall(SYS_TTYATTACH, 0, 0, 0);

    say("this is the kernel's own image, with its selftests and no "
        "system.\n");
    say("all, none, or names such as sched/wait, and timeout=SECONDS "
        "for more\n");
    say("than %d each. none leaves the machine up, with the monitor on "
        "alt+5,\n", TIMEOUT_S);
    say("or ctrl+\\ then 5 on serial\n");
    do {
        write(1, "run which selftests? ", 21);
        int64_t i_got = read(0, s_arr_answer, sizeof s_arr_answer - 1);
        s_arr_answer[i_got > 0 ? i_got : 0] = '\0';
    } while (!understand(&b_none));

    if (b_none) {
        say("no selftests. the machine stays up\n");
        idle();
    }

    for (int i_at = 0; i_at < s_i_tests; i_at++) {
        int i_status = run(s_arr_tests[i_at]);
        if (i_status == TIMED_OUT) {
            say("selftests/%s ran out of %ld seconds, and was killed\n",
                s_arr_tests[i_at], s_i_timeout_s);
            continue;
        }
        say("selftests/%s ended with %d\n", s_arr_tests[i_at], i_status);
        if (i_status == KSFT_PASS) {
            i_passed++;
        }
    }

    say("%d of %d passed. powering off\n", i_passed, s_i_tests);
    ksft_syscall(SYS_REBOOT, REBOOT_POWER_OFF, 0, 0);
    say("the machine would not power off\n");
    idle();
}

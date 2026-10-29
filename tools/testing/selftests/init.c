// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/init.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the init of the kernel's own image: does first what a system's /init
 * does, mounts the disk and makes it the root, then asks which selftests
 * to run, runs each one as a guest, the way a system runs a program, or
 * as root when only root may run it, and powers the machine off. the
 * kernel hands the machine to /init, and this is the smallest /init that
 * checks anything.
 */

#include "kselftest.h"

#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define GUEST_UID    1000       /* the account the kernel does least for */
#define ANSWER_MAX   2048
#define TESTS_MAX    128
#define NAME_MAX     64
#define TIMEOUT_S    30
#define POLL_MS      20
#define TIMED_OUT    (-1)
#define LOST         (-2)
#define DEV_MAX      24

/*
 * the disk is mounted here before it is the root, and the archive this
 * runs from goes to /boot on it, which is where the tests are then
 */
#define NEW_ROOT     "/root"
#define OLD_ROOT     "/boot"

/*
 * what ends or stops a program, which pid 1 ignores and a test is given
 * back. the two a group behind its terminal is stopped with are among
 * them, since this hands the terminal to each test and takes it back
 */
static const int s_arr_signals[] = {
    SIGINT, SIGTSTP, SIGTERM, SIGHUP, SIGTTOU, SIGTTIN,
};

static char s_arr_answer[ANSWER_MAX];
static char s_arr_found[TESTS_MAX][NAME_MAX];
static const char *s_arr_tests[TESTS_MAX];
static int s_i_tests;
static long s_i_timeout_s = TIMEOUT_S;
static const char *s_str_tests = "/selftests";

static __attribute__((format(printf, 1, 2))) void say(const char *str_fmt, ...)
{
    va_list args;

    va_start(args, str_fmt);
    ksft_print("init: ", str_fmt, args);
    va_end(args);
}

static void set_signals(void (*p_handler)(int))
{
    size_t sz_count = sizeof s_arr_signals / sizeof s_arr_signals[0];

    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        signal(s_arr_signals[sz_at], p_handler);
    }
}

static long uptime_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* where the tests are, and the name, which is where a test is */
static bool path_of(const char *str_name, char *str_path, size_t sz_size)
{
    int i_len = snprintf(str_path, sz_size, "%s/%s", s_str_tests, str_name);

    return i_len > 0 && (size_t)i_len < sz_size;
}

/* every test on the medium, subsystem by subsystem, as `subsystem/test` */
static void find_all(void)
{
    char arr_dir[NAME_MAX * 2];
    DIR *p_top = opendir(s_str_tests);

    s_i_tests = 0;
    for (struct dirent *p_sub; p_top != NULL
                               && (p_sub = readdir(p_top)) != NULL; ) {
        if (p_sub->d_type != DT_DIR || p_sub->d_name[0] == '.'
            || !path_of(p_sub->d_name, arr_dir, sizeof arr_dir)) {
            continue;
        }
        DIR *p_dir = opendir(arr_dir);
        for (struct dirent *p_test; p_dir != NULL
                                    && (p_test = readdir(p_dir)) != NULL; ) {
            size_t sz_sub = strlen(p_sub->d_name);
            size_t sz_test = strlen(p_test->d_name);
            if (p_test->d_type == DT_DIR || s_i_tests == TESTS_MAX
                || sz_sub + 1 + sz_test >= NAME_MAX) {
                continue;
            }
            char *str_out = s_arr_found[s_i_tests];
            memcpy(str_out, p_sub->d_name, sz_sub);
            str_out[sz_sub] = '/';
            memcpy(str_out + sz_sub + 1, p_test->d_name, sz_test + 1);
            s_arr_tests[s_i_tests] = s_arr_found[s_i_tests];
            s_i_tests++;
        }
        if (p_dir != NULL) {
            closedir(p_dir);
        }
    }
    if (p_top != NULL) {
        closedir(p_top);
    }
}

/*
 * the answer, split into words where it is read: `all`, `none`,
 * `timeout=SECONDS` or a test's name. true with s_arr_tests filled in,
 * or false when it has to be asked again
 */
static bool understand(bool *p_none)
{
    bool b_all = false;
    char *str_save = NULL;

    *p_none = false;
    s_i_tests = 0;
    for (char *str_word = strtok_r(s_arr_answer, " \t\n", &str_save);
         str_word != NULL; str_word = strtok_r(NULL, " \t\n", &str_save)) {
        if (ksft_streq(str_word, "all")) {
            b_all = true;
        } else if (ksft_streq(str_word, "none")) {
            *p_none = true;
        } else if (strncmp(str_word, "timeout=", 8) == 0) {
            char *p_end = NULL;
            long i_seconds = strtol(str_word + 8, &p_end, 10);
            if (*p_end != '\0' || i_seconds <= 0 || i_seconds > 100000) {
                say("%s is not a number of seconds\n", str_word);
                return false;
            }
            s_i_timeout_s = i_seconds;
        } else {
            char arr_path[NAME_MAX * 2];
            if (!path_of(str_word, arr_path, sizeof arr_path)
                || access(arr_path, F_OK) != 0) {
                say("there is no selftests/%s: %s\n", str_word,
                    strerror(errno));
                return false;
            }
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
 * a test only root may run is one the guest could not run at all, so it
 * runs as root: the file's own permissions say whose it is, and a test
 * that checks what only root may do is installed so nobody else may
 * run it
 */
static bool is_roots(const char *str_path)
{
    struct stat st;

    return stat(str_path, &st) == 0 && (st.st_mode & S_IXOTH) == 0;
}

/*
 * the child: in a group of its own, with the signals pid 1 ignores back
 * to what a program expects, and in front of the terminal before
 * anything else. a guest only after that, because nothing it does
 * should depend on having been root, unless it is a test of what only
 * root may do
 */
static __attribute__((noreturn)) void become(const char *str_path,
                                             bool b_as_root)
{
    char *arr_argv[] = { (char *)str_path, NULL };

    setpgid(0, 0);
    set_signals(SIG_DFL);
    while (tcgetpgrp(0) != getpid()) {
        ksft_sleep_ms(1);
    }
    if (!b_as_root && (setgid(GUEST_UID) != 0 || setuid(GUEST_UID) != 0)) {
        _exit(126);
    }
    execv(str_path, arr_argv);
    _exit(127);
}

/*
 * what every thread was doing when a test ran out of time, which is
 * the one thing worth knowing about a hang and the one thing gone once
 * the test is killed
 */
static void show_threads(void)
{
    static char s_arr_table[8192];
    int64_t i_len = ksft_read_file("/proc/threads", s_arr_table,
                                   sizeof s_arr_table);
    if (i_len > 0) {
        write(1, s_arr_table, (size_t)i_len);
    }
}

/*
 * one test, from its start to how it ended: its wait status, or
 * TIMED_OUT when it had to be killed
 */
static int run(const char *str_name)
{
    char arr_path[NAME_MAX * 2];
    int i_status = 0;

    path_of(str_name, arr_path, sizeof arr_path);
    bool b_as_root = is_roots(arr_path);
    if (b_as_root) {
        say("selftests/%s is root's, and runs as root\n", str_name);
    }
    say("selftests/%s starts\n", str_name);

    pid_t i_pid = fork();
    if (i_pid == 0) {
        become(arr_path, b_as_root);
    }
    if (i_pid < 0) {
        return 127 << 8;
    }
    setpgid(i_pid, i_pid);
    tcsetpgrp(0, i_pid);

    long i_until = uptime_ms() + s_i_timeout_s * 1000;
    for (;;) {
        pid_t i_got = waitpid(i_pid, &i_status, WNOHANG);
        if (i_got == i_pid) {
            break;
        }
        if (i_got < 0) {
            say("selftests/%s could not be waited for: %s\n", str_name,
                strerror(errno));
            i_status = LOST;
            break;
        }
        if (uptime_ms() >= i_until) {
            show_threads();
            kill(-i_pid, SIGKILL);
            waitpid(i_pid, &i_status, 0);
            i_status = TIMED_OUT;
            break;
        }
        ksft_sleep_ms(POLL_MS);
    }

    /* the terminal back, and whatever the test left behind collected */
    tcsetpgrp(0, getpgrp());
    while (waitpid(-1, NULL, WNOHANG) > 0) {
    }
    return i_status;
}

/*
 * a drive or a partition mounted at str_at, as ext4 or, failing that,
 * as fat, the two kinds a disk this boots from holds
 */
static bool mount_any(const char *str_dev, const char *str_at)
{
    return mount(str_dev, str_at, "ext4", 0, NULL) == 0
           || mount(str_dev, str_at, "vfat", 0, NULL) == 0;
}

static bool exists(const char *str_path)
{
    struct stat st;

    return stat(str_path, &st) == 0;
}

/*
 * the system's disk: the first partition, in the order the drives are,
 * that holds a filesystem, or a whole drive with no table that does,
 * mounted at NEW_ROOT, with its name in str_root, its drive's letter in
 * *p_drive and which partition it is in *p_part, 0 for a whole drive
 */
static bool find_disk(char *str_root, char *p_drive, int *p_part)
{
    for (char c_drive = 'a'; c_drive <= 'h'; c_drive++) {
        snprintf(str_root, DEV_MAX, "/dev/sd%c", c_drive);
        if (!exists(str_root)) {
            continue;
        }
        *p_drive = c_drive;
        snprintf(str_root, DEV_MAX, "/dev/sd%c1", c_drive);
        bool b_table = exists(str_root);
        for (int i_part = 1; b_table && i_part <= 15; i_part++) {
            snprintf(str_root, DEV_MAX, "/dev/sd%c%d", c_drive, i_part);
            if (exists(str_root) && mount_any(str_root, NEW_ROOT)) {
                *p_part = i_part;
                return true;
            }
        }
        snprintf(str_root, DEV_MAX, "/dev/sd%c", c_drive);
        if (!b_table && mount_any(str_root, NEW_ROOT)) {
            *p_part = 0;
            return true;
        }
    }
    return false;
}

/*
 * what a system's /init does first: /proc, and the disk that holds the
 * system made the root, with /dev and /proc moved onto it and the
 * archive this came from put under it at /boot, and then the next
 * partition on that drive that holds a filesystem at /work. with no
 * disk the archive stays the root
 */
static void start_system(void)
{
    char arr_dev[DEV_MAX];
    char c_drive = 'a';
    int i_part = 0;

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        say("/proc would not mount: %s\n", strerror(errno));
    }
    mkdir(NEW_ROOT, 0755);
    if (!find_disk(arr_dev, &c_drive, &i_part)) {
        say("no disk with a filesystem on it. the archive is the root\n");
        return;
    }
    say("%s is mounted at " NEW_ROOT "\n", arr_dev);

    /* the places it moves into, which a disk made elsewhere may lack */
    mkdir(NEW_ROOT OLD_ROOT, 0755);
    mkdir(NEW_ROOT "/dev", 0755);
    mkdir(NEW_ROOT "/proc", 0555);
    if (mount("/dev", NEW_ROOT "/dev", NULL, MS_MOVE, NULL) != 0
        || mount("/proc", NEW_ROOT "/proc", NULL, MS_MOVE, NULL) != 0
        || syscall(SYS_pivot_root, NEW_ROOT, NEW_ROOT OLD_ROOT) != 0) {
        say("the disk would not be the root: %s\n", strerror(errno));
        return;
    }
    chdir("/");
    s_str_tests = OLD_ROOT "/selftests";
    say("the disk is the root now, and the archive is at " OLD_ROOT "\n");

    mkdir("/work", 0755);
    for (int i_next = i_part + 1; i_part > 0 && i_next <= 15; i_next++) {
        snprintf(arr_dev, sizeof arr_dev, "/dev/sd%c%d", c_drive, i_next);
        if (exists(arr_dev) && mount_any(arr_dev, "/work")) {
            say("%s is mounted at /work\n", arr_dev);
            break;
        }
    }
}

/* pid 1 has nothing left to do but collect orphans */
static __attribute__((noreturn)) void idle(void)
{
    for (;;) {
        if (waitpid(-1, NULL, 0) < 0) {
            ksft_sleep_ms(1000);
        }
    }
}

/* how a test ended, as a shell would say it */
static void say_ended(const char *str_name, int i_status)
{
    if (WIFSIGNALED(i_status)) {
        say("selftests/%s ended by signal %d\n", str_name,
            WTERMSIG(i_status));
    } else {
        say("selftests/%s ended with %d\n", str_name,
            WEXITSTATUS(i_status));
    }
}

int main(void)
{
    bool b_none = false;
    int i_passed = 0;

    /*
     * pid 1 ending is the end of the machine, so these are not for it,
     * and the console is its terminal, to hand to each test in turn
     */
    set_signals(SIG_IGN);
    ioctl(0, TIOCSCTTY, 0);
    start_system();

    /*
     * somewhere anyone may make things, as a system has. it is on the
     * disk, so an earlier boot may have made it already
     */
    mkdir("/tmp", 01777);
    chmod("/tmp", 01777);

    say("this is the kernel's own image, with its selftests and no "
        "system.\n");
    say("all, none, or names such as sched/wait, and timeout=SECONDS "
        "for more\n");
    say("than %d each. none runs nothing, and powers off\n", TIMEOUT_S);
    do {
        write(1, "run which selftests? ", 21);
        ssize_t i_got = read(0, s_arr_answer, sizeof s_arr_answer - 1);
        s_arr_answer[i_got > 0 ? i_got : 0] = '\0';
    } while (!understand(&b_none));

    /*
     * none is a boot and a shutdown with nothing in between, which is a
     * test of its own: the kernel has no shell to leave up, and a
     * machine left running with nothing to do is one somebody has to
     * notice and stop
     */
    if (b_none) {
        say("no selftests. powering off\n");
        reboot(RB_POWER_OFF);
        say("the machine would not power off\n");
        idle();
    }

    for (int i_at = 0; i_at < s_i_tests; i_at++) {
        int i_status = run(s_arr_tests[i_at]);
        if (i_status == LOST) {
            say("selftests/%s ended with -1\n", s_arr_tests[i_at]);
            continue;
        }
        if (i_status == TIMED_OUT) {
            say("selftests/%s ran out of %ld seconds, and was killed\n",
                s_arr_tests[i_at], s_i_timeout_s);
            continue;
        }
        say_ended(s_arr_tests[i_at], i_status);
        if (WIFEXITED(i_status) && WEXITSTATUS(i_status) == KSFT_PASS) {
            i_passed++;
        }
    }

    say("%d of %d passed. powering off\n", i_passed, s_i_tests);
    reboot(RB_POWER_OFF);
    say("the machine would not power off\n");
    idle();
}

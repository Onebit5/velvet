// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/dev.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /dev: the devices a program opens by name.
 */

#include "../kselftest.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

/* the whole of a short read, or less when the device ends first */
static ssize_t read_all(const char *str_path, char *p_buf, size_t sz_len)
{
    int i_fd = open(str_path, O_RDONLY);
    if (i_fd < 0) {
        return -1;
    }
    ssize_t i_got = read(i_fd, p_buf, sz_len);
    close(i_fd);
    return i_got;
}

static bool all_zero(const char *p_buf, size_t sz_len)
{
    for (size_t sz_at = 0; sz_at < sz_len; sz_at++) {
        if (p_buf[sz_at] != 0) {
            return false;
        }
    }
    return true;
}

int main(void)
{
    char arr_a[64];
    char arr_b[64];
    struct stat st;

    ksft_print_header();
    ksft_set_plan(8);

    bool b_null = false;
    bool b_tty = false;
    DIR *p_dir = opendir("/dev");
    for (struct dirent *p_ent; p_dir != NULL
                               && (p_ent = readdir(p_dir)) != NULL; ) {
        b_null |= ksft_streq(p_ent->d_name, "null")
                  && p_ent->d_type == DT_CHR;
        b_tty |= ksft_streq(p_ent->d_name, "tty1");
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    ksft_test_result(b_null && b_tty, "/dev lists its devices\n");

    ksft_test_result(stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode)
                     && major(st.st_rdev) == 1 && minor(st.st_rdev) == 3,
                     "each a character device with its number\n");

    ksft_test_result(read_all("/dev/null", arr_a, sizeof arr_a) == 0,
                     "null reads as the end\n");

    memset(arr_a, 7, sizeof arr_a);
    ksft_test_result(read_all("/dev/zero", arr_a, sizeof arr_a) == 64
                     && all_zero(arr_a, sizeof arr_a),
                     "zero reads as zeroes\n");

    ksft_test_result(read_all("/dev/urandom", arr_a, sizeof arr_a) == 64
                     && read_all("/dev/urandom", arr_b, sizeof arr_b) == 64
                     && !all_zero(arr_a, sizeof arr_a)
                     && memcmp(arr_a, arr_b, sizeof arr_a) != 0,
                     "urandom never reads the same twice\n");

    ksft_test_result(open("/dev/nothing", O_RDONLY) == -1 && errno == ENOENT,
                     "a device it does not have does not open\n");

    int i_tty = open("/dev/tty", O_RDWR);
    ksft_test_result(i_tty >= 0 && isatty(i_tty), "/dev/tty is the "
                     "terminal of the session it is opened in\n");
    if (i_tty >= 0) {
        close(i_tty);
    }

    /* a new session starts with no terminal, until its leader takes one */
    pid_t i_child = fork();
    if (i_child == 0) {
        _exit(setsid() > 0 && open("/dev/tty", O_RDWR) == -1
              && errno == ENXIO ? 0 : 1);
    }
    int i_status = -1;
    waitpid(i_child, &i_status, 0);
    ksft_test_result(ksft_exited(i_status, 0),
                     "and nothing to a session without one\n");
    ksft_finished();
}

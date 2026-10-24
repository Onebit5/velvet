// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/dev.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /dev: the devices a program opens by name.
 */

#include "../kselftest.h"

/* the whole of a short read, or less when the device ends first */
static int64_t read_all(const char *str_path, char *p_buf, size_t sz_len)
{
    int i_fd = open(str_path);
    if (i_fd < 0) {
        return -1;
    }
    int64_t i_got = read(i_fd, p_buf, sz_len);
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

int main(int i_argc, char **pp_argv)
{
    char arr_a[64];
    char arr_b[64];
    char arr_name[64];

    (void)i_argc;
    (void)pp_argv;
    ksft_print_header();
    ksft_set_plan(5);

    bool b_null = false;
    for (size_t sz_at = 0; readdir("/dev", sz_at, arr_name, sizeof arr_name)
                           > 0; sz_at++) {
        b_null |= ksft_streq(arr_name, "null");
    }
    ksft_test_result(b_null, "/dev lists its devices\n");

    ksft_test_result(read_all("/dev/null", arr_a, sizeof arr_a) == 0,
                     "null reads as the end\n");

    for (size_t sz_at = 0; sz_at < sizeof arr_a; sz_at++) {
        arr_a[sz_at] = 7;
    }
    ksft_test_result(read_all("/dev/zero", arr_a, sizeof arr_a) == 64
                     && all_zero(arr_a, sizeof arr_a),
                     "zero reads as zeroes\n");

    ksft_test_result(read_all("/dev/urandom", arr_a, sizeof arr_a) == 64
                     && read_all("/dev/urandom", arr_b, sizeof arr_b) == 64
                     && !all_zero(arr_a, sizeof arr_a)
                     && !ksft_memeq(arr_a, arr_b, sizeof arr_a),
                     "urandom never reads the same twice\n");

    ksft_test_result(open("/dev/nothing") == -1,
                     "and a device it does not have does not open\n");
    ksft_finished();
}

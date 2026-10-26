// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/proc.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * /proc, read the way any other file is read.
 */

#include "../kselftest.h"

#include <dirent.h>

#define TEXT_MAX 2048

/*
 * the whole of a file, read `sz_step` bytes at a time: its length, or -1
 * when it would not open
 */
static int64_t slurp(const char *str_path, char *str_buf, size_t sz_size,
                     size_t sz_step)
{
    int i_fd = open(str_path, O_RDONLY);
    size_t sz_len = 0;

    if (i_fd < 0) {
        return -1;
    }
    while (sz_len < sz_size) {
        size_t sz_want = sz_size - sz_len < sz_step ? sz_size - sz_len
                                                    : sz_step;
        ssize_t i_got = read(i_fd, str_buf + sz_len, sz_want);
        if (i_got <= 0) {
            break;
        }
        sz_len += (size_t)i_got;
    }
    close(i_fd);
    return (int64_t)sz_len;
}

static bool listed(const char *str_name)
{
    bool b_found = false;
    DIR *p_dir = opendir("/proc");

    for (struct dirent *p_ent; p_dir != NULL
                               && (p_ent = readdir(p_dir)) != NULL; ) {
        b_found |= ksft_streq(p_ent->d_name, str_name);
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    return b_found;
}

/* seconds, a point, two digits and the end of the line */
static bool looks_like_uptime(const char *str_text, int64_t i_len)
{
    int64_t i_at = 0;

    while (i_at < i_len && str_text[i_at] >= '0' && str_text[i_at] <= '9') {
        i_at++;
    }
    return i_at > 0 && i_len == i_at + 4 && str_text[i_at] == '.'
           && str_text[i_at + 1] >= '0' && str_text[i_at + 1] <= '9'
           && str_text[i_at + 2] >= '0' && str_text[i_at + 2] <= '9'
           && str_text[i_at + 3] == '\n';
}

int main(void)
{
    char arr_whole[TEXT_MAX];
    char arr_bits[TEXT_MAX];

    ksft_print_header();
    ksft_set_plan(7);

    int64_t i_len = slurp("/proc/version", arr_whole, TEXT_MAX, TEXT_MAX);
    ksft_test_result(i_len > 15
                     && memcmp(arr_whole, "velvet version ", 15) == 0,
                     "/proc/version says which kernel this is\n");
    ksft_test_result(ksft_contains(arr_whole, i_len, " (gcc "),
                     "and which compiler made it\n");

    int64_t i_bits = slurp("/proc/version", arr_bits, TEXT_MAX, 3);
    ksft_test_result(i_bits == i_len
                     && memcmp(arr_whole, arr_bits, (size_t)i_len) == 0,
                     "three bytes at a time, it is the same file\n");

    i_len = slurp("/proc/uptime", arr_whole, TEXT_MAX, TEXT_MAX);
    ksft_test_result(looks_like_uptime(arr_whole, i_len),
                     "/proc/uptime is seconds, to the hundredth\n");

    ksft_test_result(listed("version") && listed("uptime"),
                     "/proc lists its files\n");
    ksft_test_result(open("/proc/nothing", O_RDONLY) == -1 && errno == ENOENT,
                     "a name /proc does not have does not open\n");
    int i_dir = open("/proc", O_RDONLY);
    ksft_test_result(i_dir >= 0 && read(i_dir, arr_bits, 1) == -1
                     && errno == EISDIR,
                     "and /proc itself is a directory, not a file\n");
    ksft_finished();
}

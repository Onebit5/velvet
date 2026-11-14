// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/disk.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the drives, the partitions and the mounts, read from /proc by a program.
 */

#include "../kselftest.h"

#include <sys/stat.h>
#include <sys/sysmacros.h>

#define TEXT_MAX 4096

static char s_arr_text[TEXT_MAX];

static int64_t slurp(const char *str_path)
{
    int64_t i_len = ksft_read_file(str_path, s_arr_text, TEXT_MAX - 1);
    s_arr_text[(i_len > 0) ? i_len : 0] = '\0';
    return i_len;
}

/*
 * every line of partitions after its heading a block device in /dev
 * with the numbers the line gives it, in columns four, seven and ten
 * wide, the name after: how many, or -1 for a line that is not
 */
static int devices_listed(void)
{
    static const char s_arr_head[] = "major minor  #blocks  name\n\n";
    if (strncmp(s_arr_text, s_arr_head, sizeof s_arr_head - 1) != 0) {
        return -1;
    }
    int i_count = 0;
    for (const char *p_at = s_arr_text + sizeof s_arr_head - 1;
         *p_at != '\0'; i_count++) {
        unsigned u_major;
        unsigned u_minor;
        unsigned long long u_kb;
        char arr_name[32];
        char arr_dev[48];
        struct stat st;
        int i_used = 0;
        if (sscanf(p_at, "%u %u %llu %31s%n", &u_major, &u_minor, &u_kb,
                   arr_name, &i_used) != 4 || p_at[i_used] != '\n'
            || i_used != 25 + (int)strlen(arr_name)) {
            return -1;
        }
        snprintf(arr_dev, sizeof arr_dev, "/dev/%s", arr_name);
        if (stat(arr_dev, &st) != 0 || !S_ISBLK(st.st_mode)
            || major(st.st_rdev) != u_major || minor(st.st_rdev) != u_minor
            || u_kb == 0) {
            return -1;
        }
        p_at += i_used + 1;
    }
    return i_count;
}

int main(void)
{
    char arr_to[64];

    ksft_print_header();
    ksft_set_plan(6);

    ssize_t i_to = readlink("/proc/mounts", arr_to, sizeof arr_to - 1);
    arr_to[(i_to > 0) ? i_to : 0] = '\0';
    int64_t i_len = slurp("/proc/mounts");
    ksft_test_result(ksft_streq(arr_to, "self/mounts")
                     && ksft_contains(s_arr_text, i_len, " /proc proc "),
                     "/proc/mounts is a link to the reader's own, which "
                     "has /proc\n");

    /* a machine with or without a disk answers, in either case */
    i_len = slurp("/proc/disk");
    bool b_drive = ksft_contains(s_arr_text, i_len, "drive      ");
    ksft_test_result(b_drive || ksft_contains(s_arr_text, i_len, "no disk."),
                     "/proc/disk names the drive, or says there is none\n");

    slurp("/proc/partitions");
    int i_devices = devices_listed();
    ksft_test_result(i_devices >= 0,
                     "/proc/partitions is a block device in /dev a line, "
                     "by its numbers\n");
    ksft_test_result(i_devices > 0 || !b_drive,
                     "and has the drives /proc/disk names\n");

    i_len = slurp("/proc/filesystems");
    ksft_test_result(ksft_contains(s_arr_text, i_len, "\text4\n")
                     && ksft_contains(s_arr_text, i_len, "nodev\tproc\n")
                     && ksft_contains(s_arr_text, i_len, "nodev\tdevpts\n"),
                     "/proc/filesystems has what mount knows, and which "
                     "need no drive\n");

    i_len = slurp("/proc/cmdline");
    ksft_test_result(i_len > 0 && s_arr_text[i_len - 1] == '\n'
                     && strchr(s_arr_text, '\n') == s_arr_text + i_len - 1,
                     "/proc/cmdline is one line\n");
    ksft_finished();
}

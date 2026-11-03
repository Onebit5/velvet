// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/fs/fsck.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the filesystem at /, checked from the machine: the partition under it
 * read by the byte, under the filesystem mounted on it, by the same
 * checker the host runs on that partition once the machine is down. the
 * checker's report is passed on whole, so what it said here can be held
 * against what it says there. this test is installed so only root may
 * run it.
 */

#include "../kselftest.h"

#include <dirent.h>
#include <sys/stat.h>

#define REPORT_MAX 2048

static char s_arr_dev[sizeof "/dev/" + 256];
static char s_arr_checker[256];

/* the drive or partition /dev names by the number the root is on */
static bool find_root_dev(void)
{
    struct stat st_root;
    struct stat st;
    bool b_found = false;

    if (stat("/", &st_root) != 0) {
        return false;
    }
    DIR *p_dir = opendir("/dev");
    for (struct dirent *p_ent; p_dir != NULL && !b_found
                               && (p_ent = readdir(p_dir)) != NULL; ) {
        snprintf(s_arr_dev, sizeof s_arr_dev, "/dev/%s", p_ent->d_name);
        b_found = p_ent->d_type == DT_BLK && stat(s_arr_dev, &st) == 0
                  && S_ISBLK(st.st_mode) && st.st_rdev == st_root.st_dev;
    }
    if (p_dir != NULL) {
        closedir(p_dir);
    }
    return b_found;
}

/*
 * the checker, in bin/ of the archive this test came from: three names
 * up from the test, which is selftests/fs/fsck in it
 */
static bool find_checker(const char *str_me)
{
    snprintf(s_arr_checker, sizeof s_arr_checker, "%s", str_me);
    for (int i_up = 0; i_up < 3; i_up++) {
        char *p_slash = strrchr(s_arr_checker, '/');
        if (p_slash == NULL) {
            return false;
        }
        *p_slash = '\0';
    }
    size_t sz_len = strlen(s_arr_checker);
    snprintf(s_arr_checker + sz_len, sizeof s_arr_checker - sz_len,
             "/bin/fsckext4");
    return access(s_arr_checker, X_OK) == 0;
}

/* the checker run on the partition, its report and its exit status */
static int check(char *str_report, size_t sz_size)
{
    int arr_pipe[2];
    size_t sz_at = 0;

    if (pipe(arr_pipe) != 0) {
        return -1;
    }
    pid_t i_child = fork();
    if (i_child == 0) {
        char *arr_argv[] = { s_arr_checker, "-n", s_arr_dev, NULL };
        dup2(arr_pipe[1], 1);
        dup2(arr_pipe[1], 2);
        close(arr_pipe[0]);
        execv(s_arr_checker, arr_argv);
        _exit(127);
    }
    close(arr_pipe[1]);
    for (ssize_t i_got; sz_at + 1 < sz_size
                        && (i_got = read(arr_pipe[0], str_report + sz_at,
                                         sz_size - 1 - sz_at)) > 0; ) {
        sz_at += (size_t)i_got;
    }
    str_report[sz_at] = '\0';
    close(arr_pipe[0]);
    int i_status = -1;
    waitpid(i_child, &i_status, 0);
    return WIFEXITED(i_status) ? WEXITSTATUS(i_status) : -1;
}

int main(int i_argc, char **pp_argv)
{
    static char s_arr_report[REPORT_MAX];

    (void)i_argc;
    ksft_print_header();
    ksft_set_plan(4);

    /* what the filesystem holds in memory, out to the partition first */
    sync();
    bool b_dev = find_root_dev();
    ksft_test_result(b_dev, "the root is on %s, by the number its own "
                     "device has\n", s_arr_dev);
    bool b_checker = find_checker(pp_argv[0]);
    ksft_test_result(b_checker, "the checker is on the medium, at %s\n",
                     s_arr_checker);

    int i_status = (b_dev && b_checker)
                   ? check(s_arr_report, sizeof s_arr_report) : -1;
    bool b_clean = strstr(s_arr_report, "nothing wrong with it") != NULL;
    /* the report, a line at a time, marked so the boot test can find it */
    for (char *str_line = strtok(s_arr_report, "\n"); str_line != NULL;
         str_line = strtok(NULL, "\n")) {
        ksft_print_msg("checker: %s\n", str_line);
    }
    /* it exits 2 when it could not make the check at all */
    ksft_test_result(i_status >= 0 && i_status != 2 && i_status != 127,
                     "it reads the partition under the mounted filesystem "
                     "to the end\n");
    ksft_test_result(i_status == 0 && b_clean,
                     "and finds nothing wrong with it\n");
    ksft_finished();
}

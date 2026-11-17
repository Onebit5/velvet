// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/loader.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program linked against a libc's shared library, and the libc's
 * loader, both as musl makes them for any machine with this interface.
 * the loader is built to be loaded anywhere and is started as a program
 * of its own here, and it loads the other, which says what it found.
 * then the program is started by its own name, which the kernel starts
 * the loader it names for, and copies of it name other loaders, there
 * and not, runnable and not, programs and not.
 */

#include "../kselftest.h"

#include <sys/stat.h>

#define LOADER  "/lib/ld-musl-x86_64.so.1"
#define PROGRAM "/bin/dynamic"

/* a copy of the program naming another loader: one as long as LOADER */
#define RENAMED   "/tmp/loader-renamed"
#define ELSEWHERE "/tmp/ld-musl-x86_64.so.1"

/* room for the whole of the loader, the larger of the two */
static char s_arr_file[2 << 20];

/*
 * str_path run with pp_argv, its standard output, and its standard error
 * with it, into str_out: how it ended, or -1 when it could not be run
 */
static int run(const char *str_path, char *const *pp_argv, char *str_out,
               size_t sz_size)
{
    int arr_pipe[2];
    int i_status = -1;
    str_out[0] = '\0';
    if (pipe(arr_pipe) != 0) {
        return -1;
    }
    pid_t i_pid = fork();
    if (i_pid == 0) {
        dup2(arr_pipe[1], 1);
        dup2(arr_pipe[1], 2);
        close(arr_pipe[0]);
        execv(str_path, pp_argv);
        dprintf(1, "refused %d\n", errno);
        _exit(127);
    }
    close(arr_pipe[1]);
    size_t sz_len = 0;
    for (ssize_t i_got = 1; i_got > 0 && sz_len < sz_size - 1;
         sz_len += (size_t)((i_got > 0) ? i_got : 0)) {
        i_got = read(arr_pipe[0], str_out + sz_len, sz_size - 1 - sz_len);
    }
    str_out[sz_len] = '\0';
    close(arr_pipe[0]);
    if (i_pid > 0) {
        waitpid(i_pid, &i_status, 0);
    }
    return i_status;
}

/* whether str_text has a line that is str_line, whole */
static bool says(const char *str_text, const char *str_line)
{
    size_t sz_len = strlen(str_line);
    for (const char *p_at = str_text; (p_at = strstr(p_at, str_line)) != NULL;
         p_at++) {
        if ((p_at == str_text || p_at[-1] == '\n') && p_at[sz_len] == '\n') {
            return true;
        }
    }
    return false;
}

/* whether the value of str_key, on its line, ends with str_tail */
static bool ends(const char *str_text, const char *str_key,
                 const char *str_tail)
{
    char arr_find[64];
    snprintf(arr_find, sizeof arr_find, "\n%s ", str_key);
    const char *p_at = strstr(str_text, arr_find);
    if (p_at == NULL) {
        return false;
    }
    p_at += strlen(arr_find);
    size_t sz_value = strcspn(p_at, "\n");
    size_t sz_tail = strlen(str_tail);
    return sz_value >= sz_tail
           && memcmp(p_at + sz_value - sz_tail, str_tail, sz_tail) == 0;
}

/* whether a run was refused by execv with i_errno, and never started */
static bool refused(int i_status, const char *str_out, int i_errno)
{
    char arr_line[32];
    snprintf(arr_line, sizeof arr_line, "refused %d", i_errno);
    return ksft_exited(i_status, 127) && says(str_out, arr_line);
}

/* sz_len bytes of s_arr_file as the whole of str_to, with i_mode */
static bool put(const char *str_to, size_t sz_len, int i_mode)
{
    unlink(str_to);
    int i_fd = open(str_to, O_WRONLY | O_CREAT | O_TRUNC, i_mode);
    bool b_ok = i_fd >= 0 && write(i_fd, s_arr_file, sz_len)
                                 == (ssize_t)sz_len;
    close(i_fd);
    return b_ok;
}

/* str_from into s_arr_file: how long it is, or 0 */
static size_t take(const char *str_from)
{
    int64_t i_len = ksft_read_file(str_from, s_arr_file, sizeof s_arr_file);
    return (i_len > 0 && (size_t)i_len < sizeof s_arr_file) ? (size_t)i_len
                                                            : 0;
}

/*
 * a copy of the program at RENAMED naming ELSEWHERE as its loader, the
 * name ended as a path is, or with its last byte something else
 */
static bool renamed(bool b_ended)
{
    size_t sz_len = take(PROGRAM);
    size_t sz_name = sizeof LOADER;
    for (size_t sz_at = 0; sz_at + sz_name <= sz_len; sz_at++) {
        if (memcmp(s_arr_file + sz_at, LOADER, sz_name) == 0) {
            memcpy(s_arr_file + sz_at, ELSEWHERE, sz_name);
            s_arr_file[sz_at + sz_name - 1] = b_ended ? '\0' : '/';
            return put(RENAMED, sz_len, 0755);
        }
    }
    return false;
}

/* how a run that went wrong ended, and what it said, before its result */
static void tell(int i_status, const char *str_out)
{
    ksft_print_msg("it ended with status %#x, saying:\n", i_status);
    for (const char *p_at = str_out; *p_at != '\0';) {
        size_t sz_line = strcspn(p_at, "\n");
        ksft_print_msg("  %.*s\n", (int)sz_line, p_at);
        p_at += sz_line + (p_at[sz_line] == '\n');
    }
}

int main(void)
{
    static char s_arr_out[8192];

    ksft_print_header();
    ksft_set_plan(15);

    char *arr_alone[] = { LOADER, NULL };
    int i_status = run(LOADER, arr_alone, s_arr_out, sizeof s_arr_out);
    bool b_ok = ksft_exited(i_status, 1)
                && strstr(s_arr_out, "Dynamic Program Loader") != NULL;
    if (!b_ok) {
        tell(i_status, s_arr_out);
    }
    ksft_test_result(b_ok, "the loader, built to be loaded anywhere, starts "
                     "and says what it is\n");

    char *arr_load[] = { LOADER, PROGRAM, NULL };
    i_status = run(LOADER, arr_load, s_arr_out, sizeof s_arr_out);
    b_ok = ksft_exited(i_status, 0) && says(s_arr_out, "relocated 1");
    if (!b_ok) {
        tell(i_status, s_arr_out);
    }
    ksft_test_result(b_ok, "and loads a program of its own, which it fixes "
                     "up\n");
    ksft_test_result(says(s_arr_out, "moved 1"),
                     "the program is put somewhere else than it was "
                     "linked at\n");
    ksft_test_result(ends(s_arr_out, "started", LOADER)
                     && says(s_arr_out, "described 1"),
                     "the auxiliary vector describes the program the kernel "
                     "started, the loader, where it put it\n");
    ksft_test_result(ends(s_arr_out, "self", PROGRAM)
                     && ends(s_arr_out, "libc", "/ld-musl-x86_64.so.1"),
                     "maps names the program, and the libc it uses\n");

    char *arr_named[] = { PROGRAM, "two", NULL };
    i_status = run(PROGRAM, arr_named, s_arr_out, sizeof s_arr_out);
    b_ok = ksft_exited(i_status, 0) && says(s_arr_out, "relocated 1")
           && says(s_arr_out, "moved 1");
    if (!b_ok) {
        tell(i_status, s_arr_out);
    }
    ksft_test_result(b_ok, "a program naming a loader is started by it, "
                     "which fixes it up\n");
    ksft_test_result(ends(s_arr_out, "started", PROGRAM)
                     && says(s_arr_out, "described 1"),
                     "the auxiliary vector describes the program, where "
                     "the kernel put it\n");
    ksft_test_result(!says(s_arr_out, "base 0")
                     && ends(s_arr_out, "loader", LOADER),
                     "and says where the loader was put, which maps "
                     "names\n");
    ksft_test_result(says(s_arr_out, "arg " PROGRAM)
                     && says(s_arr_out, "arg two"),
                     "the arguments are the program's, as it was started "
                     "with them\n");

    char *arr_renamed[] = { RENAMED, NULL };
    unlink(ELSEWHERE);
    b_ok = renamed(true);
    i_status = run(RENAMED, arr_renamed, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_ok && refused(i_status, s_arr_out, ENOENT),
                     "a loader named but not there is ENOENT, though the "
                     "program is\n");
    b_ok = put(ELSEWHERE, take(LOADER), 0644);
    i_status = run(RENAMED, arr_renamed, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_ok && refused(i_status, s_arr_out, EACCES),
                     "one the caller may not run is EACCES\n");
    chmod(ELSEWHERE, 0755);
    i_status = run(RENAMED, arr_renamed, s_arr_out, sizeof s_arr_out);
    b_ok = ksft_exited(i_status, 0) && ends(s_arr_out, "loader", ELSEWHERE);
    if (!b_ok) {
        tell(i_status, s_arr_out);
    }
    ksft_test_result(b_ok, "and one it may is found by the name the program "
                     "gives, wherever that is\n");
    memset(s_arr_file, '-', 4096);
    b_ok = put(ELSEWHERE, 4096, 0755);
    i_status = run(RENAMED, arr_renamed, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_ok && refused(i_status, s_arr_out, ELIBBAD),
                     "one that is no program at all is ELIBBAD\n");
    b_ok = put(ELSEWHERE, 63, 0755);
    i_status = run(RENAMED, arr_renamed, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_ok && refused(i_status, s_arr_out, EIO),
                     "and one too short to hold a header is EIO, its "
                     "header unread\n");
    b_ok = renamed(false);
    i_status = run(RENAMED, arr_renamed, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_ok && refused(i_status, s_arr_out, ENOEXEC),
                     "and a name that does not end is no program's, "
                     "ENOEXEC\n");
    unlink(RENAMED);
    unlink(ELSEWHERE);
    ksft_finished();
}

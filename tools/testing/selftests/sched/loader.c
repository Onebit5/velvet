// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/loader.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a program linked against a libc's shared library, and the libc's
 * loader, both as musl makes them for any machine with this interface.
 * the loader is built to be loaded anywhere and is started as a program
 * of its own here, and it loads the other, which says what it found.
 */

#include "../kselftest.h"

#define LOADER  "/lib/ld-musl-x86_64.so.1"
#define PROGRAM "/bin/dynamic"

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
    ksft_set_plan(5);

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
    ksft_finished();
}

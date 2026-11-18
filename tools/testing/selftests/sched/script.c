// SPDX-License-Identifier: GPL-2.0-only
/*
 * tools/testing/selftests/sched/script.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a script: a file whose first line names the program that runs it.
 * the test writes scripts naming the test itself, which, started with
 * the right argument after it, says what it was started with, and holds
 * that against what a script's program is promised.
 */

#define _GNU_SOURCE
#include "../kselftest.h"

#include <sys/auxv.h>
#include <sys/stat.h>

/*
 * what a script's line puts after the program's name, for it to know it
 * was started by one: the start of its first argument
 */
#define SAY "--say"

/* how deep scripts may name scripts before the last is refused */
#define DEPTH 5

static char s_arr_self[256];
static char s_arr_dir[64];

/* started by a script: everything it was started with, a line each */
static int say(int i_argc, char **pp_argv)
{
    static char s_arr_comm[64];
    char arr_exe[256];
    int64_t i_len = ksft_read_file("/proc/self/comm", s_arr_comm,
                                   sizeof s_arr_comm - 1);
    s_arr_comm[(i_len > 0) ? i_len : 0] = '\0';
    ssize_t i_exe = readlink("/proc/self/exe", arr_exe, sizeof arr_exe - 1);
    arr_exe[(i_exe > 0) ? i_exe : 0] = '\0';
    for (int i_at = 0; i_at < i_argc; i_at++) {
        printf("arg %s\n", pp_argv[i_at]);
    }
    printf("comm %s", s_arr_comm);
    printf("exe %s\n", arr_exe);
    printf("execfn %s\n", (const char *)getauxval(AT_EXECFN));
    return 0;
}

/*
 * str_path run with pp_argv, its standard output and error into str_out,
 * and execv's errno as a line of its own when it was refused: how it
 * ended, or -1 when it could not be run
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

/* whether a run was refused by execv with i_errno, and never started */
static bool refused(int i_status, const char *str_out, int i_errno)
{
    char arr_line[32];
    snprintf(arr_line, sizeof arr_line, "refused %d\n", i_errno);
    return ksft_exited(i_status, 127) && strcmp(str_out, arr_line) == 0;
}

/* a file at str_path holding str_text, with i_mode */
static bool put(const char *str_path, const char *str_text, int i_mode)
{
    unlink(str_path);
    int i_fd = open(str_path, O_WRONLY | O_CREAT | O_TRUNC, i_mode);
    size_t sz_len = strlen(str_text);
    bool b_ok = i_fd >= 0 && write(i_fd, str_text, sz_len)
                                 == (ssize_t)sz_len;
    close(i_fd);
    return b_ok;
}

/* the name of a file of the test's own in its directory, numbered */
static const char *named(char *str_out, size_t sz_size, const char *str_kind,
                         int i_at)
{
    snprintf(str_out, sz_size, "%s/%s%d", s_arr_dir, str_kind, i_at);
    return str_out;
}

/* str_path run with no more arguments than its name, into str_out */
static int run_alone(const char *str_path, char *str_out, size_t sz_size)
{
    char *arr_args[] = { (char *)str_path, NULL };
    return run(str_path, arr_args, str_out, sz_size);
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

/* whether str_out is str_want, and if not, says so before the result */
static bool said(int i_status, const char *str_out, const char *str_want)
{
    if (ksft_exited(i_status, 0) && strcmp(str_out, str_want) == 0) {
        return true;
    }
    ksft_print_msg("wanted:\n");
    tell(0, str_want);
    tell(i_status, str_out);
    return false;
}

int main(int i_argc, char **pp_argv)
{
    static char s_arr_out[8192];
    static char s_arr_want[8192];
    char arr_path[256];
    char arr_text[1024];
    char arr_last[256];

    if (i_argc > 1 && strncmp(pp_argv[1], SAY, strlen(SAY)) == 0) {
        return say(i_argc, pp_argv);
    }
    ksft_print_header();
    ksft_set_plan(12);

    ssize_t i_self = readlink("/proc/self/exe", s_arr_self,
                              sizeof s_arr_self - 1);
    s_arr_self[(i_self > 0) ? i_self : 0] = '\0';
    snprintf(s_arr_dir, sizeof s_arr_dir, "/tmp/script-%d", (int)getpid());
    mkdir(s_arr_dir, 0755);

    /*
     * the line: blanks around the name and around the argument dropped,
     * and blanks inside the argument kept, for it is one argument
     */
    named(arr_path, sizeof arr_path, "s", 0);
    snprintf(arr_text, sizeof arr_text, "#! \t%s " SAY " two\t \nnot read\n",
             s_arr_self);
    bool b_put = put(arr_path, arr_text, 0755);
    char *arr_args[] = { "s0", "x", "y", NULL };
    int i_status = run(arr_path, arr_args, s_arr_out, sizeof s_arr_out);
    snprintf(s_arr_want, sizeof s_arr_want,
             "arg %s\narg " SAY " two\narg %s\narg x\narg y\n"
             "comm s0\nexe %s\nexecfn %s\n",
             s_arr_self, arr_path, s_arr_self, arr_path);
    ksft_test_result(b_put && said(i_status, s_arr_out, s_arr_want),
                     "a script is run by the program its line names, with "
                     "the line's one argument and the script's name before "
                     "its own, and named for the script\n");

    /* its name as it was asked for, not as the kernel found it */
    chdir(s_arr_dir);
    char *arr_here[] = { "./s0", NULL };
    i_status = run("./s0", arr_here, s_arr_out, sizeof s_arr_out);
    snprintf(s_arr_want, sizeof s_arr_want,
             "arg %s\narg " SAY " two\narg ./s0\n"
             "comm s0\nexe %s\nexecfn ./s0\n", s_arr_self, s_arr_self);
    ksft_test_result(said(i_status, s_arr_out, s_arr_want),
                     "the name it is handed is the one it was asked for "
                     "by\n");
    chdir("/");

    /* a tab ends the name as a space does */
    snprintf(arr_text, sizeof arr_text, "#!%s\t" SAY "\n", s_arr_self);
    b_put = put(named(arr_path, sizeof arr_path, "t", 0), arr_text, 0755);
    i_status = run_alone(arr_path, s_arr_out, sizeof s_arr_out);
    snprintf(s_arr_want, sizeof s_arr_want,
             "arg %s\narg " SAY "\narg %s\ncomm t0\nexe %s\nexecfn %s\n",
             s_arr_self, arr_path, s_arr_self, arr_path);
    ksft_test_result(b_put && said(i_status, s_arr_out, s_arr_want),
                     "a tab ends the program's name as a space does\n");

    /*
     * scripts naming scripts, each one's line before the next's, as deep
     * as DEPTH, and one deeper is refused
     */
    snprintf(arr_last, sizeof arr_last, "%s", s_arr_self);
    s_arr_want[0] = '\0';
    b_put = true;
    for (int i_at = 1; i_at <= DEPTH + 1; i_at++) {
        snprintf(arr_text, sizeof arr_text, "#!%s %s\n", arr_last,
                 (i_at == 1) ? SAY : "on");
        b_put = put(named(arr_path, sizeof arr_path, "d", i_at), arr_text,
                    0755) && b_put;
        snprintf(arr_last, sizeof arr_last, "%s", arr_path);
    }
    i_status = run_alone(named(arr_path, sizeof arr_path, "d", DEPTH),
                         s_arr_out, sizeof s_arr_out);
    size_t sz_want = (size_t)snprintf(s_arr_want, sizeof s_arr_want,
                                      "arg %s\narg " SAY "\n", s_arr_self);
    for (int i_at = 1; i_at <= DEPTH; i_at++) {
        sz_want += (size_t)snprintf(s_arr_want + sz_want,
                                    sizeof s_arr_want - sz_want,
                                    "arg %s\n%s",
                                    named(arr_path, sizeof arr_path, "d",
                                          i_at),
                                    (i_at < DEPTH) ? "arg on\n" : "");
    }
    snprintf(s_arr_want + sz_want, sizeof s_arr_want - sz_want,
             "comm d%d\nexe %s\nexecfn %s\n", DEPTH, s_arr_self,
             named(arr_path, sizeof arr_path, "d", DEPTH));
    ksft_test_result(b_put && said(i_status, s_arr_out, s_arr_want),
                     "a script may name a script, five deep\n");
    i_status = run_alone(named(arr_path, sizeof arr_path, "d", DEPTH + 1),
                         s_arr_out, sizeof s_arr_out);
    ksft_test_result(refused(i_status, s_arr_out, ELOOP),
                     "and six deep is ELOOP\n");

    /* lines that name nothing that could be run */
    b_put = put(named(arr_path, sizeof arr_path, "e", 0), "#!\n", 0755);
    i_status = run_alone(arr_path, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_put && refused(i_status, s_arr_out, ENOEXEC),
                     "a line naming nothing is ENOEXEC\n");
    b_put = put(named(arr_path, sizeof arr_path, "e", 1), "#! \t \n", 0755);
    i_status = run_alone(arr_path, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_put && refused(i_status, s_arr_out, ENOEXEC),
                     "and so is one of blanks\n");
    b_put = put(named(arr_path, sizeof arr_path, "e", 3), "#!", 0755);
    i_status = run_alone(arr_path, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_put && refused(i_status, s_arr_out, EACCES),
                     "but one with no end names what is up to the end of "
                     "the file, here the empty name, where the caller "
                     "stands, which is no program: EACCES\n");
    snprintf(arr_text, sizeof arr_text, "#!/%0300d", 0);
    b_put = put(named(arr_path, sizeof arr_path, "e", 2), arr_text, 0755);
    i_status = run_alone(arr_path, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_put && refused(i_status, s_arr_out, ENOEXEC),
                     "and so is one whose name does not end within the "
                     "first 256 bytes, as it may go on past them\n");

    /* an argument that does not end there is cut where they end */
    snprintf(arr_text, sizeof arr_text, "#!%s " SAY "%0300d", s_arr_self,
             0);
    b_put = put(named(arr_path, sizeof arr_path, "c", 0), arr_text, 0755);
    i_status = run_alone(arr_path, s_arr_out, sizeof s_arr_out);
    arr_text[255] = '\0';
    snprintf(s_arr_want, sizeof s_arr_want,
             "arg %s\narg %s\narg %s\ncomm c0\nexe %s\nexecfn %s\n",
             s_arr_self, arr_text + 3 + strlen(s_arr_self), arr_path,
             s_arr_self, arr_path);
    ksft_test_result(b_put && said(i_status, s_arr_out, s_arr_want),
                     "an argument that goes on past them is cut short\n");

    /* the program a line names is looked for as any program is */
    snprintf(arr_text, sizeof arr_text, "#!%s/gone\n", s_arr_dir);
    b_put = put(named(arr_path, sizeof arr_path, "n", 0), arr_text, 0755);
    i_status = run_alone(arr_path, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_put && refused(i_status, s_arr_out, ENOENT),
                     "a program named that is not there is ENOENT\n");
    snprintf(arr_text, sizeof arr_text, "#!%s\n", arr_path);
    b_put = put(named(arr_last, sizeof arr_last, "n", 1), arr_text, 0755)
            && chmod(arr_path, 0644) == 0;
    i_status = run_alone(arr_last, s_arr_out, sizeof s_arr_out);
    ksft_test_result(b_put && refused(i_status, s_arr_out, EACCES),
                     "and one that may not be run is EACCES\n");

    for (int i_at = 0; i_at <= DEPTH + 1; i_at++) {
        static const char *const s_arr_kinds[] = { "s", "t", "d", "e", "c",
                                                   "n" };
        for (size_t sz_kind = 0; sz_kind < 6; sz_kind++) {
            unlink(named(arr_path, sizeof arr_path, s_arr_kinds[sz_kind],
                         i_at));
        }
    }
    rmdir(s_arr_dir);
    ksft_finished();
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/usermode.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * how big a stack ring 3 may grow to, and how much of it exists before the
 * program starts.
 */

/* the design notes for usermode.h are in docs/subsystems/mm.rst */

#ifndef VELVET_USERMODE_H
#define VELVET_USERMODE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define USER_STACK_PAGES 256            /* a megabyte of room */
#define USER_STACK_EAGER 2              /* mapped before it starts */

#define USER_STACK_TOP 0x0000700000000000ull

#define MAX_ARGS 8

/* implemented in usermode.asm. never returns */
void enter_usermode(uint64_t entry, uint64_t stack_top,
                    uint64_t cs, uint64_t ss,
                    uint64_t argc, uint64_t argv);

extern const char *const USER_RUN_NO_SUCH_FILE;

/* where a program starts, for whoever is about to send a thread there */
struct user_entry {
    uint64_t u_entry;
    uint64_t u_stack_top;
    uint64_t u_argc;
    uint64_t u_argv;
};

/*
 * the calling process becomes the program at `str_path`, keeping its
 * pid, descriptors, working directory, uid and environment, with its
 * caught signals back to their defaults. true with `p_out` saying where
 * the new program starts; false with nothing changed at all
 */
bool user_exec(const char *str_path, int i_argc, const char *const pp_argv[],
               struct user_entry *p_out, const char **pp_error);

/*
 * take an ended process off the table, its pipes let go of first:
 * whether there was one to take, and its status in `*p_code`
 */
bool user_collect(int i_pid, int *p_code);

struct pipe;

/* where a program's standard input and output go, before it starts. */
struct spawn_env {
    const char *block;
    size_t      len;
};

struct spawn_io {
    struct pipe *in;            /* a pipe to read from */
    struct pipe *out;           /* a pipe to write to */
    const char  *in_path;       /* `< name`: read from this file */
    const char  *out_path;      /* `> name`: write to this file */
    bool         append;        /* `>>`: keep what is there and add to it */
};

int user_spawn(const char *path, int argc, const char *const argv[],
               const char *cwd,
               int parent, int uid, bool announce,
               const struct spawn_io *io, const char **error);

void user_spawn_env(const struct spawn_env *env);

#define PIPELINE_MAX 4

struct stage {
    const char *path;
    int         argc;
    char      **argv;

    /*
     * `<`, `>` and `>>` belong to a single command rather than to the
     * line, which is why they live here. `sort < a.txt > b.txt` is one
     * stage with both ends moved
     */
    const char *in_path;
    const char *out_path;
    bool        append;
};

/*
 * a spawned process is created asleep, so that whoever started it can
 * settle its group, its descriptors and who holds the terminal before
 * anything runs. this is what lets it go
 */
void user_start(int pid);

struct job {
    int  pgid;
    int  pids[PIPELINE_MAX];
    int  count;
    bool stopped;       /* suspended by ctrl+z rather than finished */

    int  status;
};

bool user_job_wait(struct job *j);

void user_job_continue(struct job *j, bool foreground);

bool user_job_alive(const struct job *j);

/*
 * collect whatever has finished, so the table does not fill with the
 * remains of jobs nobody asked about
 */
void user_job_collect(struct job *j);

bool user_pipeline(const struct stage *stages, int count, const char *cwd,
                   int uid, bool background, struct job *out,
                   const char **error);

bool user_wait(int pid, int *code);

bool user_run(const char *path, int argc, const char *const argv[],
              const char *cwd,
              int uid, bool background, bool announce, struct job *out,
              const char **error);

#endif

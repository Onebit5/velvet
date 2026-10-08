// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/usermode.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * how big a stack ring 3 may grow to, and how much of it exists before the
 * program starts.
 */

/* the design notes for usermode.h are in docs/subsystems/mm.rst */

#ifndef SCHED_USERMODE_H
#define SCHED_USERMODE_H

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

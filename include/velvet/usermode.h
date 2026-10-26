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

/*
 * the most a program may be started with, its arguments and environment
 * together, strings and the pointers to them, as they will lie on its
 * stack
 */
#define EXEC_ARGS_MAX (128 * 1024)

/* what a program is started with: argv and envp, each ending in NULL */
struct exec_args {
    const char *const *pp_argv;
    size_t sz_argc;
    const char *const *pp_envp;
    size_t sz_envc;
    char *p_strings;            /* what the two point into, when copied */
};

/*
 * the arguments and environment a program in ring 3 passed, copied into
 * the kernel before the memory they are in goes away: 0, -EFAULT, -E2BIG
 * or -ENOMEM. a NULL vector is an empty one. exec_args_free lets go
 */
int64_t exec_args_copy(uint64_t u_argv, uint64_t u_envp,
                       struct exec_args *p_out);
void exec_args_free(struct exec_args *p_args);

/* where a program starts, for whoever is about to send a thread there */
struct user_entry {
    uint64_t u_entry;
    uint64_t u_stack_top;
};

/*
 * the calling process becomes the program at `str_path`, keeping its
 * pid, descriptors, working directory and uid, with its caught signals
 * back to their defaults. 0 with `p_out` saying where the new program
 * starts, or minus an errno with nothing changed at all
 */
int64_t user_exec(const char *str_path, const struct exec_args *p_args,
                  struct user_entry *p_out);

/*
 * take an ended process off the table, its pipes let go of first:
 * whether there was one to take, and its status in `*p_code`
 */
bool user_collect(int i_pid, int *p_code);

/*
 * a new process running the program at `path`, as `uid`, the child of
 * `parent`, standing in `cwd`, with the first console on 0, 1 and 2. it
 * is parked until user_start. its pid, or 0 with the reason in `*error`
 */
int user_spawn(const char *path, const struct exec_args *p_args,
               const char *cwd, int parent, int uid, bool announce,
               const char **error);

/*
 * a spawned process is created asleep, so that whoever started it can
 * settle its group, its descriptors and who holds the terminal before
 * anything runs. this is what lets it go
 */
void user_start(int pid);

#endif

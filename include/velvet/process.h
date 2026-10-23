// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/process.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a process is a program someone started, and it outlives the thread that
 * ran it.
 */

#ifndef VELVET_PROCESS_H
#define VELVET_PROCESS_H

#include <stdint.h>
#include <velvet/signal.h>
#include <stddef.h>
#include <stdbool.h>
#include <velvet/path.h>

struct pipe;

#define MAX_PROCESSES   32
#define PROC_NAME_MAX   24

#define PROC_ENV_MAX    1024
#define ENV_NAME_MAX    64
#define ENV_VALUE_MAX   256

#define MAX_FDS         64
#define FD_STDIN        0
#define FD_STDOUT       1
#define FD_STDERR       2
#define FD_FIRST_FILE   3

#define PROCESS_KILLED  (-1)

/*
 * init.
 *
 * pid 1 is a convention everywhere and it is a convention for a reason:
 * reparenting needs a number that is known before the process it names
 * exists. this table hands out pids in order from 1 and init is the
 * first thing in it, so the two agree by construction rather than by
 * anybody remembering to keep them in step.
 *
 * pid *0* is the other special number and is not a process at all: it is
 * the parent of what the kernel starts itself, which is /init
 */
#define INIT_PID        1

struct file;

struct process {
    int      pid;               /* 0 means the slot is free */
    int      parent;            /* who started it, 0 for the kernel */

    int      pgid;
    int      uid;               /* who it runs as. 0 is the master */

    bool     announce;
    int      thread_id;         /* the thread running it, while it lives */
    char     name[PROC_NAME_MAX];
    bool     exited;
    int      exit_code;

    bool     interrupted;

    int      i_stopped_by;      /* the signal that stopped it, 0 running */
    bool     b_stop_told;       /* and whether a wait has said so yet */

    struct signal_state sig;

    uint64_t sig_trampoline;

    uint64_t alarm_at;

    int      killed_by;
    uint64_t started_ms;
    uint64_t ended_ms;
    /*
     * what each descriptor is open on, NULL for one that is not. the
     * file is shared, with whatever other descriptors were duplicated or
     * forked from this one, and its position with it
     */
    struct file *fds[MAX_FDS];
    uint64_t u_cloexec;         /* bit n: descriptor n closes on exec */

    /*
     * one block of "NAME=value" strings, each ended by a NUL, with an
     * empty string for the end of the lot. that shape is not nostalgia:
     * it is what makes the whole thing one memcpy to inherit, and
     * inheriting is most of what an environment is *for*. a table of
     * pointers would need every one of them rewritten on the way into a
     * child.
     *
     * it belongs to the process rather than to the program, which is
     * why `export` in every shell there has ever been is a builtin and
     * not a command, a command could only ever change its own
     */
    char env[PROC_ENV_MAX];
    size_t env_len;

    char cwd[PATH_MAX];
};

const char *process_cwd(int pid);
void        process_set_cwd(int pid, const char *path);

int  process_create(const char *name, int parent, int uid, bool announce,
                    uint64_t now_ms);

bool process_announces(int pid);

int  process_uid(int pid);

int  process_pgid(int pid);

/*
 * a forked child made its parent's copy: the group, the working
 * directory, the environment, the signal dispositions and mask, and the
 * trampoline its handlers return through. not what is pending: a child
 * is not born owing its parent's interrupts
 */
void process_inherit(int i_child, int i_parent);

/* who a process runs as from now on. whether it may is the caller's question */
void process_set_uid(int i_pid, int i_uid);
void process_set_pgid(int pid, int pgid);

/* the live members of a group, by pid: how many, up to `sz_max` */
size_t process_group_pids(int i_pgid, int *p_pids, size_t sz_max);

void process_set_thread(int pid, int thread_id);

int  process_thread(int pid);

/*
 * a process that has ended and not been collected is holding a slot, and
 * there are thirty-two of them. normally its parent collects it, that
 * is what `wait` is, but a parent can die first, and then the exit code
 * is addressed to nobody and the slot is held forever.
 *
 * so children are reparented to init as their parent goes, here, since
 * the moment the parent ends is the only moment anybody could notice.
 * one born to a parent that has already ended is init's from birth, and
 * init collects them all with `wait`
 */
void process_exited(int pid, int code, uint64_t now_ms);

bool process_collect(int pid, int *code);

size_t process_interrupt_all(void);

/*
 * the thread ids of every process still going, for a shutdown that has
 * run out of patience. gathered under the lock and acted on afterwards,
 * because killing a thread reaches the scheduler and a holder of this
 * lock may not
 */
size_t process_running_threads(int *ids, size_t max);

const struct process *process_find(int pid);

const char *process_name(int pid);

/* what a process is called from now on, which exec changes */
void process_set_name(int i_pid, const char *str_name);

bool process_set_env(int pid, const char *block, size_t len);

size_t process_get_env(int pid, char *out, size_t max);

bool process_env_get(int pid, const char *name, char *out, size_t max);

bool process_env_set(int pid, const char *name, const char *value);

/* split out into lib/env.c, where it is pure arithmetic over a block */
bool env_block_get(const char *block, size_t len, const char *name,
                   char *out, size_t max);
bool env_block_set(char *block, size_t *len, size_t max, const char *name,
                   const char *value);

void process_interrupt(int pid);

bool process_signal(int pid, int sig);

/* a fault's signal, which a handler may catch and nothing may refuse */
void process_force_signal(int pid, int sig);
bool process_signal_group(int pgid, int sig);

/* stopped by `i_sig` on its way back to ring 3, rather than when sent */
void process_stopped(int i_pid, int i_sig);

/*
 * a child of `i_parent` with something to say, `i_pid` itself or any
 * when it is -1: its pid, with `*p_ended` true and the exit status, or
 * false and WAIT_STOPPED with the signal for a stop not yet told, when
 * `b_untraced` asks about stops. 0 when none has anything yet, and -1
 * when there is no such child at all
 */
int process_reportable(int i_parent, int i_pid, bool b_untraced,
                       int *p_status, bool *p_ended);

struct signal_state *process_signal_state(int pid);

enum signal_action process_take_signal(int pid, int *sig_out,
                                       uint64_t *handler_out);

void process_set_trampoline(int pid, uint64_t at);
uint64_t process_trampoline(int pid);

uint64_t process_set_alarm(int pid, uint64_t seconds, uint64_t now_ms);

void process_check_alarms(uint64_t now_ms);

void process_died_by_signal(int pid, int sig);

int  process_killed_by(int pid);

bool process_interrupt_pending(int pid);

bool process_take_interrupt(int pid);

/*
 * p_file at the lowest descriptor free from i_from up, which keeps the
 * caller's reference: the descriptor, or -EMFILE with the reference
 * still the caller's to put
 */
int process_fd_add(int i_pid, struct file *p_file, int i_from,
                   bool b_cloexec);

/* the file a descriptor is open on, with a reference to put: or NULL */
struct file *process_fd_file(int i_pid, int i_fd);

/*
 * p_file at i_fd, keeping the caller's reference, and whatever was open
 * there handed back through pp_old to be put with no lock held. false
 * for a descriptor out of range, with nothing changed
 */
bool process_fd_replace(int i_pid, int i_fd, struct file *p_file,
                        bool b_cloexec, struct file **pp_old);

/* a descriptor closed: the file it was open on, to be put, or NULL */
struct file *process_fd_take(int i_pid, int i_fd);

/* the child holds what the parent holds: the same files, not copies */
bool process_fds_inherit(int pid, int from);

/*
 * every descriptor closed, as a process ends, or the ones marked to close
 * on exec. the files are put here, so neither may be called with a lock
 * held
 */
void process_fds_release(int i_pid);
void process_fds_exec(int i_pid);

size_t process_fd_count(int pid);

const struct process *process_at(size_t index);
size_t process_count(void);

#endif

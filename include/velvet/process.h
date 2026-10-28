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
#include <uapi/velvet/resource.h>

struct pipe;

#define MAX_PROCESSES   32
#define PROC_NAME_MAX   24

/* the supplementary groups one process may be in */
#define NGROUPS_MAX     16

#define MAX_FDS         64
#define FD_STDIN        0
#define FD_STDOUT       1
#define FD_STDERR       2
#define FD_FIRST_FILE   3

/* the wait status of a process the kernel ended rather than a signal */
#define PROCESS_KILLED  SIGKILL

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
    int      i_sid;             /* the session: its leader's pid */
    int      uid;               /* who it runs as. 0 is the master */
    int      i_gid;
    int      arr_groups[NGROUPS_MAX];
    int      i_groups;
    uint32_t u_umask;           /* the mode bits a new file does not get */
    struct rlimit arr_limits[RLIM_NLIMITS];

    bool     announce;
    int      thread_id;         /* the thread running it, while it lives */
    char     name[PROC_NAME_MAX];
    bool     exited;
    bool     b_detached;        /* its parent ignores SIGCHLD: nobody waits */
    int      i_wait_status;     /* what wait4 reports: W_EXITCODE */

    bool     interrupted;

    int      i_stopped_by;      /* the signal that stopped it, 0 running */
    bool     b_stop_told;       /* and whether a wait has said so yet */
    bool     b_continued;       /* continued and not yet told so */

    struct signal_state sig;

    uint64_t alarm_at;          /* when ITIMER_REAL is due, ms since boot */
    uint64_t u_alarm_every;     /* and how often after that, 0 for once */
    uint64_t u_clear_tid;       /* where exit writes 0, set_tid_address's */
    bool     b_vforked;         /* its parent waits until it execs or ends */

    uint64_t started_ms;
    uint64_t ended_ms;

    /*
     * the ticks it ran for, told when it ends, and those of the children
     * it has collected: all of it user time, the two never being told
     * apart
     */
    uint64_t u_ticks;
    uint64_t u_child_ticks;
    /*
     * what each descriptor is open on, NULL for one that is not. the
     * file is shared, with whatever other descriptors were duplicated or
     * forked from this one, and its position with it
     */
    struct file *fds[MAX_FDS];
    uint64_t u_cloexec;         /* bit n: descriptor n closes on exec */

    char cwd[PATH_MAX];
};

/* the process the running thread belongs to, 0 for a kernel thread */
int current_pid(void);

const char *process_cwd(int pid);
void        process_set_cwd(int pid, const char *path);

int  process_create(const char *name, int parent, int uid, bool announce,
                    uint64_t now_ms);

bool process_announces(int pid);

int  process_uid(int pid);

int  process_pgid(int pid);

/*
 * a forked child made its parent's copy: the group and session, who it
 * runs as, its umask and limits, the working directory, and the signal
 * actions, mask and alternate stack. not what is pending: a child is not
 * born owing its parent's interrupts
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

/* the ticks a process's thread ran for, as it ends */
void process_account(int i_pid, uint64_t u_ticks);

/*
 * an ended process nobody will wait for, its parent ignoring SIGCHLD,
 * taken off the table once its thread is gone. any other is left for
 * its parent
 */
void process_forget(int i_pid);

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

void process_interrupt(int pid);

bool process_signal(int pid, int sig);

/* one from somebody: a kill, or a child telling its parent it changed */
bool process_send_signal(int pid, int sig,
                         const struct signal_origin *origin);

/*
 * a fault's signal, which a handler may catch and nothing may refuse,
 * with where and why the fault was
 */
void process_force_signal(int pid, int sig,
                          const struct signal_origin *origin);

/*
 * a vfork's child has let its parent go, by running another program or
 * by ending, and whether it still has not
 */
void process_vfork_done(int i_pid);
bool process_vforking(int i_pid);
bool process_signal_group(int pgid, int sig);

/* stopped by `i_sig` on its way back to ring 3, rather than when sent */
void process_stopped(int i_pid, int i_sig);

/*
 * a child of i_parent's that the selector picks, the way wait4 picks
 * one: a pid, -1 for any, 0 for any in the caller's group, minus a group
 * for any in that one. with something to say: its pid and its wait
 * status, *p_ended true when it has ended and is the caller's to
 * collect. a stop is told with WUNTRACED and a continue with
 * WCONTINUED, each once, and WNOWAIT leaves either to be told again.
 * 0 when there are such children and none has anything yet, -ECHILD
 * when there are none
 */
int process_reportable(int i_parent, int i_selector, int i_options,
                       int *p_status, bool *p_ended);

struct signal_state *process_signal_state(int pid);

/*
 * i_pid's signal mask set to u_mask: the one it replaced. b_suspend also
 * keeps that one for whichever handler ends the wait to go back to, as
 * sigsuspend, ppoll and pselect6 wait. what the new mask lets through
 * that was already pending interrupts the wait at once
 */
uint64_t process_set_mask(int i_pid, uint64_t u_mask, bool b_suspend);

/*
 * the lowest pending signal that is in u_set taken off without being
 * delivered, as sigtimedwait takes one, with who sent it: which, or 0
 * when none of them is pending
 */
int process_take_from(int i_pid, uint64_t u_set,
                      struct signal_origin *p_origin);

/* the next deliverable signal taken off, as signal_take takes it */
enum signal_action process_take_signal(int pid, int *sig_out,
                                       struct sigaction *act_out,
                                       uint64_t *old_mask_out,
                                       struct signal_origin *origin_out);

/*
 * ITIMER_REAL: SIGALRM u_value_ms from now and every u_every_ms after,
 * 0 for never, with what was set before in the two outs, which may be
 * NULL
 */
void process_set_timer(int i_pid, uint64_t u_value_ms, uint64_t u_every_ms,
                       uint64_t u_now_ms, uint64_t *p_old_value,
                       uint64_t *p_old_every);

void process_check_alarms(uint64_t now_ms);


bool process_interrupt_pending(int pid);

bool process_take_interrupt(int pid);

/*
 * a process's slot with the table locked, to read or change what has no
 * call of its own, and the lock let go of after. the slot is NULL for a
 * pid that is not there, with the lock taken all the same. nothing that
 * sleeps, or wakes anybody, may be done in between
 */
struct process *process_lock_slot(int i_pid, uint64_t *p_flags);
void process_unlock_slot(uint64_t u_flags);

/*
 * a descriptor's close-on-exec bit: what it was, 0 or 1, set to i_set
 * unless that is -1. -EBADF for a descriptor not open
 */
int process_fd_cloexec(int i_pid, int i_fd, int i_set);

/* how many descriptors i_pid may have open, its RLIMIT_NOFILE */
int process_fd_limit(int i_pid);

/*
 * p_file at the lowest descriptor free from i_from up and below the
 * limit, which keeps the caller's reference: the descriptor, or -EMFILE
 * with the reference still the caller's to put
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

/*
 * whether any process has a file open, or a working directory, that
 * p_test says yes to. p_test is called with the process table locked,
 * and may take nothing of its own
 */
bool process_any_file(bool (*p_test)(const struct file *p_file,
                                      const void *p_ctx),
                      const void *p_ctx);
bool process_any_cwd(bool (*p_test)(const char *str_cwd, const void *p_ctx),
                     const void *p_ctx);

const struct process *process_at(size_t index);
size_t process_count(void);

#endif

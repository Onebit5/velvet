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
#include <velvet/thread.h>
#include <uapi/velvet/resource.h>

struct pipe;

#define MAX_PROCESSES   32
#define PROC_NAME_MAX   24

/* the most threads there may be, every process's together */
#define MAX_THREADS     64

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
    char     name[PROC_NAME_MAX];
    bool     exited;
    bool     b_detached;        /* its parent ignores SIGCHLD: nobody waits */
    int      i_wait_status;     /* what wait4 reports: W_EXITCODE */
    bool     b_group_exit;      /* every thread is on its way out */

    int      i_stopped_by;      /* the signal that stopped it, 0 running */
    bool     b_stop_told;       /* and whether a wait has said so yet */
    bool     b_continued;       /* continued and not yet told so */

    struct signal_state sig;    /* every thread's actions, and its pending */

    uint64_t alarm_at;          /* when ITIMER_REAL is due, ms since boot */
    uint64_t u_alarm_every;     /* and how often after that, 0 for once */
    bool     b_vforked;         /* its parent waits until it execs or ends */

    uint64_t started_ms;
    uint64_t ended_ms;

    /*
     * what its threads had of the cpus, each told as it ends, and what the
     * children it has collected had, theirs included
     */
    struct cpu_usage used;
    struct cpu_usage children;
    /*
     * what each descriptor is open on, NULL for one that is not. the
     * file is shared, with whatever other descriptors were duplicated or
     * forked from this one, and its position with it
     */
    struct file *fds[MAX_FDS];
    uint64_t u_cloexec;         /* bit n: descriptor n closes on exec */

    char cwd[PATH_MAX];

    /*
     * where its "/" is, empty for the namespace's own: a name it hands
     * over is resolved inside this, and cannot climb out of it
     */
    char arr_root[PATH_MAX];
};

/*
 * a thread of a process, as the table keeps it: what is the thread's own
 * rather than its process's. a process's first thread has the process's
 * pid for its tid, and every other is numbered out of the same count, so
 * a tid is never somebody else's pid
 */
struct task {
    int      i_tid;             /* 0 means the slot is free */
    int      i_pid;             /* the process it is a thread of */
    int      i_thread;          /* the scheduler's thread, 0 until it has one */
    bool     b_interrupted;     /* a signal it has to look at */
    bool     b_doomed;          /* to end at its next way back to ring 3 */
    uint64_t u_clear_tid;       /* where its end writes 0, set_tid_address's */
    struct signal_thread sig;
};

/* the process the running thread belongs to, 0 for a kernel thread */
int current_pid(void);

/* and which of its threads that is, 0 for a kernel thread */
int current_tid(void);

const char *process_cwd(int pid);
void        process_set_cwd(int pid, const char *path);

/* where a process's "/" is in the namespace, "/" for none of its own */
const char *process_root(int i_pid);
void        process_set_root(int i_pid, const char *str_path);

int  process_create(const char *name, int parent, int uid, bool announce,
                    uint64_t now_ms);

bool process_announces(int pid);

int  process_uid(int pid);

int  process_pgid(int pid);

/*
 * a forked child made a copy of thread i_from's process: the group and
 * session, who it runs as, its umask and limits, the working directory,
 * the signal actions, and that thread's mask and alternate stack. not
 * what is pending: a child is not born owing its parent's interrupts
 */
void process_inherit(int i_child, int i_from);

/* who a process runs as from now on. whether it may is the caller's question */
void process_set_uid(int i_pid, int i_uid);
void process_set_pgid(int pid, int pgid);

/* the live members of a group, by pid: how many, up to `sz_max` */
size_t process_group_pids(int i_pgid, int *p_pids, size_t sz_max);

/*
 * whether no member of a group has a parent in another group of its
 * session, which leaves nobody with job control over it
 */
bool process_group_orphaned(int i_pgid);

/* which of the scheduler's threads runs thread i_tid, and which that is */
void process_set_thread(int i_tid, int i_thread);
int  process_thread(int i_tid);

/* the process thread i_tid is a thread of, 0 for none */
int  process_pid_of(int i_tid);

/*
 * another thread for the process thread i_from is a thread of, with
 * i_from's mask, and no alternate stack, the one it has being in memory
 * the two share: its tid, or 0 when there is no room for it or the
 * process is on its way out
 */
int  process_add_thread(int i_from);

/* how many threads a process has */
size_t process_thread_count(int i_pid);

/*
 * thread i_tid has ended, with code for its wait status: true when it
 * was its process's last, and the process has ended with it. the status
 * a process ends with is its group exit's, when it had one, and
 * otherwise its first thread's
 */
bool process_thread_ended(int i_tid, int i_code, uint64_t u_now_ms);

/*
 * every thread of thread i_tid's process but i_tid doomed: woken, let
 * go of if stopped, and ended on its way back to ring 3. with b_exec it
 * is so i_tid can run another program alone, and otherwise it is the
 * whole process ending with i_code, the first group exit's code being
 * the one it ends with. false when i_tid is doomed itself, which is
 * every thread but one while a thread is running a program, so only one
 * may be doing either at a time
 */
bool process_doom_others(int i_tid, int i_code, bool b_exec);

/* whether a thread is doomed, and is to end rather than go back */
bool process_doomed(int i_tid);

/*
 * thread i_tid, alone in its process, becomes the process's first,
 * with the process's pid for its tid, as exec leaves the thread that
 * ran it
 */
void process_lead(int i_tid);

/*
 * the scheduler's threads of every thread of i_pid, stopped or let go,
 * which is what stopping or continuing a process is
 */
void process_set_stopped(int i_pid, bool b_stopped);

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

/* what one of a process's threads had of the cpus, added as it ends */
void process_account(int i_pid, const struct cpu_usage *p_used);

/*
 * what the threads of a process that have ended had of the cpus, and in
 * *p_children, unless that is NULL, what the children it has collected
 * had
 */
struct cpu_usage process_ended_usage(int i_pid, struct cpu_usage *p_children);

/*
 * an ended process nobody will wait for, its parent ignoring SIGCHLD,
 * taken off the table once its threads are gone. any other is left for
 * its parent
 */
void process_forget(int i_pid);

size_t process_interrupt_all(void);

/*
 * the scheduler's thread of every thread of every process still going,
 * for a shutdown that has run out of patience. gathered under the lock
 * and acted on afterwards, because killing a thread reaches the
 * scheduler and a holder of this lock may not
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
 * a fault's signal, raised at the thread that made it, which a handler
 * may catch and nothing may refuse, with where and why the fault was
 */
void process_force_signal(int i_tid, int sig,
                          const struct signal_origin *origin);

/*
 * one raised at a single thread rather than its whole process, as tkill
 * and a write to a pipe nobody reads send one
 */
bool process_tkill(int i_tid, int i_sig, const struct signal_origin *origin);

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

/* a process's half of its signal state, and a thread's */
struct signal_state *process_signal_state(int pid);
struct signal_thread *process_thread_signals(int i_tid);

/*
 * an action set for every thread of i_pid, with the one it replaced in
 * *p_old: 0, or -EINVAL for one a signal may not have. one ignored from
 * now on is gone from every set it was pending in
 */
int process_set_action(int i_pid, int i_sig, const struct sigaction *p_act,
                       struct sigaction *p_old);

/*
 * thread i_tid's signal mask set to u_mask: the one it replaced.
 * b_suspend also keeps that one for whichever handler ends the wait to
 * go back to, as sigsuspend, ppoll and pselect6 wait. what the new mask
 * lets through that was already pending interrupts the wait at once
 */
uint64_t process_set_mask(int i_tid, uint64_t u_mask, bool b_suspend);

/*
 * the mask a wait put in place given back, with no handler having run to
 * give it back on returning: what a wait does that ends some other way
 */
void process_unsuspend(int i_tid);

/* what is pending for thread i_tid and blocked, which sigpending tells */
uint64_t process_pending(int i_tid);

/*
 * whether thread i_tid would be stopped by, or would have to handle, a
 * signal sent to it: one it neither blocks nor ignores
 */
bool process_would_take(int i_tid, int i_sig);

/*
 * the lowest pending signal that is in u_set taken off without being
 * delivered, as sigtimedwait takes one, with who sent it: which, or 0
 * when none of them is pending
 */
int process_take_from(int i_tid, uint64_t u_set,
                      struct signal_origin *p_origin);

/* the next signal thread i_tid takes, taken off as signal_take takes it */
enum signal_action process_take_signal(int i_tid, int *sig_out,
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


/*
 * whether thread i_tid has a signal to look at, which is what ends a
 * call that would otherwise wait, and the same with the flag cleared
 */
bool process_interrupt_pending(int i_tid);

bool process_take_interrupt(int i_tid);

/*
 * a process's slot with the table locked, to read or change what has no
 * call of its own, and the lock let go of after. the slot is NULL for a
 * pid that is not there, with the lock taken all the same. nothing that
 * sleeps, or wakes anybody, may be done in between
 */
struct process *process_lock_slot(int i_pid, uint64_t *p_flags);
void process_unlock_slot(uint64_t u_flags);

/* and a thread's, the same way, let go of with process_unlock_slot */
struct task *process_lock_task(int i_tid, uint64_t *p_flags);

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
 * every descriptor closed, once a process has ended, or the ones marked
 * to close on exec. the files are put here, so neither may be called
 * with a lock held
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

/*
 * every path a process holds rewritten in place by p_map: each one's
 * working directory and root, with b_place, and the name each open file
 * was opened by, once for each file however many descriptors share it.
 * called with the process table locked, so p_map may take nothing
 */
void process_map_paths(void (*p_map)(char *str_path, bool b_place,
                                     const void *p_ctx),
                       const void *p_ctx);

const struct process *process_at(size_t index);
size_t process_count(void);

#endif

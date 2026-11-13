// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/sched.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * round robin preemptive scheduler. one core, one run queue, no
 * priorities, no fairness accounting.
 */

/* the design notes for sched.h are in docs/subsystems/mm.rst */

#ifndef VELVET_SCHED_H
#define VELVET_SCHED_H

#include <stdint.h>
#include <velvet/thread.h>
#include <stddef.h>
#include <stdbool.h>

/* round robin preemptive scheduler. */

void sched_init(void);

void sched_yield(void);

void sleep_ms(uint64_t ms);

uint64_t sched_quantum_ms(void);

/*
 * a tick of this core's timer, which found the running thread in ring 3
 * when b_user, and in the kernel otherwise
 */
void sched_tick(bool b_user);

struct thread *sched_current(void);

/*
 * hold everything the running thread prints in `p_cap` until it is called
 * again with NULL. see struct kcapture
 */
struct kcapture;
void sched_capture(struct kcapture *p_cap);

void sched_add(struct thread *t);

struct waitq {
    struct thread *head;
};

void waitq_block(struct waitq *q);

void waitq_enqueue(struct waitq *q);
void waitq_sleep(void);

/*
 * on the queue the same way, and woken u_ms from now if nobody wakes it
 * first, 0 being never: a wait with a time limit, which a sleep that
 * polled for its wake would only answer late
 */
void waitq_enqueue_for(struct waitq *q, uint64_t u_ms);

/*
 * given back by a thread on its very first run, because it starts
 * holding a lock whose release is on a stack it will never return to
 */
void sched_first_run(void);

bool sched_join(unsigned cpu, const char *idle_name);

/* which core a thread is on, or -1 if it is not running anywhere */
int sched_thread_cpu(const struct thread *t);

/*
 * the cores taking work, a bit each, and how many. a thread goes to
 * whichever has room, so these are also the cores any thread may run on
 */
uint64_t sched_cores_mask(void);
size_t sched_cores_scheduling(void);

const char *sched_cpu_running(unsigned cpu);

void waitq_wake_all(struct waitq *q);

void sched_dump(void);

/*
 * the threads, and any pipe still open: what `ps` prints, and what
 * /proc/threads is
 */
void sched_show_threads(void);

/*
 * /proc/threads, and /proc/stack, root's: the kernel's call trace of
 * whoever reads it
 */
void sched_proc_init(void);

bool sched_thread_alive(int id);

/*
 * what a process has had of the cpus, its threads still running and the
 * ones that have ended, and in *p_children, unless that is NULL, what
 * the children it has collected had
 */
struct cpu_usage sched_process_usage(int i_pid, struct cpu_usage *p_children);

/* what a thread has had of the cpus, handed to its process as it ends */
void sched_account(struct thread *t);

/* what the scheduler knows of one thread, for whoever reports on it */
struct sched_info {
    enum thread_state e_state;
    bool b_stopped;
    struct cpu_usage cpu;
    int i_cpu;                  /* where it last ran, -1 for never */
    char arr_name[THREAD_NAME_MAX];
};

/* thread i_id's, into p_out: false for no such thread */
bool sched_info(int i_id, struct sched_info *p_out);

/*
 * the space a process's threads run in, with a use of it the caller lets
 * go of with addrspace_put: NULL for one with no thread left in ring 3
 */
struct addrspace *sched_space_of(int i_pid);

/*
 * thread t, the caller, running in p_new from now on: the space it ran
 * in, whose use is the caller's to let go of. under the lock, so nobody
 * finding the thread finds a space it has let go of
 */
struct addrspace *sched_swap_space(struct thread *t, struct addrspace *p_new);

/* one core's ticks since it joined, by what each found, and its switches */
struct sched_cpu_stat {
    struct cpu_usage cpu;       /* a thread's, in ring 3 or the kernel */
    uint64_t u_idle;            /* and nobody's */
    uint64_t u_switches;
};

/* core u_cpu's, into p_out: false for one that is not scheduling */
bool sched_cpu_stat(unsigned u_cpu, struct sched_cpu_stat *p_out);

/*
 * the load averaged over one minute, five and fifteen, in the fixed
 * point of <velvet/loadavg.h>
 */
void sched_load(uint64_t arr_load[3]);

/* how many threads want a cpu now, and in *p_threads how many there are */
size_t sched_runnable(size_t *p_threads);

/*
 * the kernel's calls that led thread i_id to where it is, printed: the
 * caller's own, or one parked, and nothing for one running on another
 * core, whose stack is changing as it is read
 */
void sched_show_stack(int i_id);

enum sched_kill_result {
    SCHED_KILL_OK,
    SCHED_KILL_NO_SUCH,
    SCHED_KILL_SELF,        /* the caller asked to end itself */
    SCHED_KILL_PROTECTED,   /* idle, somebody has to take the cpu */
};

enum sched_kill_result sched_kill(int id);

void sched_wake_thread(int id);

void sched_set_stopped(int id, bool stopped);
bool sched_thread_stopped(int id);

void waitq_remove(struct waitq *q, struct thread *t);

#endif

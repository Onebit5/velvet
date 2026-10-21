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

void sched_tick(void);

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
 * given back by a thread on its very first run, because it starts
 * holding a lock whose release is on a stack it will never return to
 */
void sched_first_run(void);

bool sched_join(unsigned cpu, const char *idle_name);

/* which core a thread is on, or -1 if it is not running anywhere */
int sched_thread_cpu(const struct thread *t);

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

// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/thread.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * threads, their stacks, and their states.
 */

/* the design notes for thread.h are in docs/subsystems/mm.rst */

#ifndef VELVET_THREAD_H
#define VELVET_THREAD_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

struct addrspace;
struct waitq;

#define THREAD_NAME_MAX  16
#define THREAD_STACK_PAGES 4        /* 16k of kernel stack each, plenty */

enum thread_state {
    THREAD_READY,       /* wants the cpu */
    THREAD_RUNNING,     /* has the cpu */
    THREAD_SLEEPING,    /* waiting for a tick to come around */
    THREAD_BLOCKED,     /* parked on a waitq until somebody says otherwise */
    THREAD_DEAD,        /* finished, waiting to be reaped */
};

struct kcapture;

struct thread {
    uint64_t sp;

    uint64_t stack_phys;        /* what the pmm gave the kernel, for giving back */
    size_t   stack_pages;

    enum thread_state state;

    bool     stopped;

    uint64_t wake_at;           /* tick to wake on, when SLEEPING */

    uint64_t cpu_ticks;

    void (*entry)(void *);
    void *arg;

    /*
     * ring 3 threads only. the address space owns every page in its
     * lower half, the program's image and its stack alike, so
     * there is nothing else to free by hand
     */
    struct addrspace *space;

    int  id;

    unsigned console;

    char name[THREAD_NAME_MAX];

    int  on_cpu;

    bool from_heap;

    struct thread *next;        /* circular run queue */

    struct waitq  *waiting_on;
    struct thread *wait_next;

    int pid;

    /* where this thread's kprintf goes instead of the screen, while set. */
    struct kcapture *p_capture;

    /*
     * how many interrupt handlers this thread is inside. a handler runs on
     * whichever thread it interrupted, so without this its prints would
     * land in that thread's capture. counted on the thread rather than the
     * core because a handler can switch threads before it returns
     */
    unsigned u_in_interrupt;
};

const char *thread_state_name(enum thread_state s);

void thread_set_name(struct thread *t, const char *name);

struct thread *thread_create(const char *name, void (*entry)(void *), void *arg);

struct thread *thread_create_parked(const char *name, void (*entry)(void *),
                                    void *arg);

void thread_free(struct thread *t);

void thread_free_stack(struct thread *t);

/* leave, with something to say about how it went. never returns */
void thread_exit(int code) __attribute__((noreturn));

#endif

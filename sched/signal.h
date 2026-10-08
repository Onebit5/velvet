// SPDX-License-Identifier: GPL-2.0-only
/*
 * sched/signal.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * signals, and what may be done with them.
 */

/* the design notes for signal.h are in docs/subsystems/sched.rst */

#ifndef SCHED_SIGNAL_H
#define SCHED_SIGNAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * signals. not a message: there is no queue and no payload, a signal is a
 * *bit* in a word, so raising one that is already raised changes nothing
 * and two ctrl+c presses in the same instant are one interrupt. a process
 * either ignores it, takes the default action, or has a handler to run,
 * and the default is nearly always die, the exceptions being the
 * interesting ones, SIGCHLD arrives constantly and a default of death
 * would mean every shell exiting the moment a command finished. SIGKILL
 * and SIGSTOP may not be caught, blocked or ignored, which is the entire
 * reason a machine can still be saved by somebody who can type.
 *
 * delivery is the easy half. every blocking call here was written assuming
 * it finishes for one of two reasons, what it waited for happened or the
 * thing it waited on went away, and a signal is a third, so a call that
 * does not know about it either sleeps through the interrupt or returns as
 * though it succeeded. a blocked thread carrying a deliverable signal is
 * therefore woken and the call returns "interrupted" rather than a result,
 * and that has to reach the program, a read that returns 0 on an interrupt
 * being indistinguishable from end of file
 */

#define SIGHUP   1
#define SIGINT   2      /* ctrl+c */
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGFPE   8
#define SIGKILL  9      /* cannot be caught, blocked or ignored */
#define SIGSEGV 11
#define SIGPIPE 13      /* wrote to a pipe nobody is reading */
#define SIGALRM 14
#define SIGTERM 15      /* the polite "please stop" */
#define SIGCHLD 17      /* a child finished. ignored by default */
#define SIGCONT 18
#define SIGSTOP 19      /* cannot be caught, blocked or ignored */
#define SIGTSTP 20      /* ctrl+z */

#define SIGNAL_MAX 32

#define SIG_DEFAULT 0
#define SIG_IGNORE  1

enum signal_action {
    SIGNAL_ACTION_NONE,     /* nothing to do */
    SIGNAL_ACTION_TERMINATE,
    SIGNAL_ACTION_IGNORE,
    SIGNAL_ACTION_STOP,
    SIGNAL_ACTION_CONTINUE,
    SIGNAL_ACTION_HANDLER   /* run the program's own, in ring 3 */
};

struct signal_state {
    uint32_t pending;       /* one bit per signal. a set, not a queue */
    uint32_t blocked;       /* deferred, not discarded */

    uint64_t handler[SIGNAL_MAX];

    int      running;
    uint32_t saved_mask;
};

void signal_reset(struct signal_state *s);

/*
 * what a process inherits across fork: the handlers and the mask, but
 * *not* the pending set. a child is not born owing its parent's
 * interrupts
 */
void signal_inherit(struct signal_state *child,
                    const struct signal_state *parent);

/* and across exec: handlers go back to default, because the code they pointed at is gone. */
void signal_exec(struct signal_state *s);

/* raise one. false if it is not a signal number */
bool signal_raise(struct signal_state *s, int sig);

int signal_next(const struct signal_state *s);

enum signal_action signal_action_for(const struct signal_state *s, int sig);

enum signal_action signal_take(struct signal_state *s, int sig,
                               uint64_t *handler_out);

void signal_handler_returned(struct signal_state *s);

/*
 * install one. false when the signal may not be caught, which is a
 * refusal the program should see rather than a lie it should believe
 */
bool signal_set_handler(struct signal_state *s, int sig, uint64_t handler);

/*
 * block or unblock. SIGKILL and SIGSTOP silently stay unblocked, the
 * rule is not negotiable and refusing the whole call would break
 * programs that block everything as a matter of course
 */
void signal_set_mask(struct signal_state *s, uint32_t mask);
uint32_t signal_get_mask(const struct signal_state *s);

bool signal_deliverable(const struct signal_state *s);

const char *signal_name(int sig);

#endif

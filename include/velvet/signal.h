// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/signal.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * signals, and what may be done with them.
 */

/* the design notes for signal.h are in docs/subsystems/sched.rst */

#ifndef VELVET_SIGNAL_H
#define VELVET_SIGNAL_H

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

#include <uapi/velvet/signal.h>

/* a signal's bit in a set: signal n is bit n - 1, as a sigset_t has it */
#define SIGNAL_BIT(sig) (1ull << ((sig) - 1))

enum signal_action {
    SIGNAL_ACTION_NONE,     /* nothing to do */
    SIGNAL_ACTION_TERMINATE,
    SIGNAL_ACTION_IGNORE,
    SIGNAL_ACTION_STOP,
    SIGNAL_ACTION_CONTINUE,
    SIGNAL_ACTION_HANDLER   /* run the program's own, in ring 3 */
};

/*
 * who raised a signal and why, kept for each pending one and handed to
 * a handler as its siginfo. a set holds one of each signal, so a second
 * raise before the first is taken replaces the first's origin
 */
struct signal_origin {
    int32_t  i_code;            /* SI_USER, SI_KERNEL, CLD_EXITED, ... */
    int32_t  i_pid;             /* the sender, or the child */
    uint32_t u_uid;
    int32_t  i_status;          /* a child's exit code, or its signal */
    uint64_t u_addr;            /* where a fault was */
};

struct signal_state {
    uint64_t pending;           /* one bit per signal. a set, not a queue */
    uint64_t blocked;           /* deferred, not discarded */

    struct sigaction action[SIGNAL_MAX];
    struct signal_origin origin[SIGNAL_MAX];

    stack_t altstack;           /* where an SA_ONSTACK handler runs */

    /*
     * rt_sigsuspend waits under a mask of the caller's choosing, and the
     * mask it replaced is the one the handler that ends the wait returns
     * to, so it is kept here until that handler's frame is made
     */
    uint64_t u_suspend_mask;
    bool     b_suspended;
};

void signal_reset(struct signal_state *s);

/*
 * what a process inherits across fork: the actions, the mask and the
 * alternate stack, but *not* the pending set. a child is not born owing
 * its parent's interrupts
 */
void signal_inherit(struct signal_state *child,
                    const struct signal_state *parent);

/*
 * and across exec: handlers go back to default, because the code they
 * pointed at is gone, and so does the alternate stack. an ignored signal
 * stays ignored, which is how nohup works
 */
void signal_exec(struct signal_state *s);

/* raise one, from origin, or from the kernel when it is NULL */
bool signal_raise(struct signal_state *s, int sig,
                  const struct signal_origin *origin);

int signal_next(const struct signal_state *s);

enum signal_action signal_action_for(const struct signal_state *s, int sig);

/*
 * take a pending signal off the set. for a handler: its action, the mask
 * the handler interrupts, which its frame keeps for sigreturn, and the
 * handler's own mask put in place, its signal in it unless SA_NODEFER,
 * and the action back to default when SA_RESETHAND said so
 */
enum signal_action signal_take(struct signal_state *s, int sig,
                               struct sigaction *act_out,
                               uint64_t *old_mask_out,
                               struct signal_origin *origin_out);

/*
 * set an action. false when it may not be: SIGKILL and SIGSTOP take no
 * handler and cannot be ignored, a refusal the program should see
 * rather than a lie it should believe
 */
bool signal_set_action(struct signal_state *s, int sig,
                       const struct sigaction *act);

/*
 * block or unblock. SIGKILL and SIGSTOP silently stay unblocked, the
 * rule is not negotiable and refusing the whole call would break
 * programs that block everything as a matter of course
 */
void signal_set_mask(struct signal_state *s, uint64_t mask);
uint64_t signal_get_mask(const struct signal_state *s);

/*
 * what a handler with SA_SIGINFO, or sigtimedwait, is told about a
 * signal: who sent it and why, a child's news, or where a fault was
 */
void signal_info(int i_sig, const struct signal_origin *p_origin,
                 struct siginfo *p_info);

/* is there anything deliverable at all? */
bool signal_deliverable(const struct signal_state *s);

const char *signal_name(int sig);

#endif

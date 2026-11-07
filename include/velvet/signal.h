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

/* signals raised and not yet taken, and who raised each */
struct signal_set {
    uint64_t u_pending;         /* one bit per signal. a set, not a queue */
    struct signal_origin arr_origin[SIGNAL_MAX];
};

/*
 * a process's half: what each signal does, which every thread of the
 * process shares, and the signals raised at the process as a whole,
 * which go to whichever of its threads takes one first
 */
struct signal_state {
    struct sigaction arr_action[SIGNAL_MAX];
    struct signal_set shared;
};

/*
 * and each thread's: what it has blocked, the signals raised at it
 * alone, a fault's and a tkill's, and where its handlers run
 */
struct signal_thread {
    uint64_t u_blocked;         /* deferred, not discarded */
    struct signal_set own;
    stack_t altstack;           /* where an SA_ONSTACK handler runs */

    /*
     * rt_sigsuspend waits under a mask of the caller's choosing, and the
     * mask it replaced is the one the handler that ends the wait returns
     * to, so it is kept here until that handler's frame is made
     */
    uint64_t u_suspend_mask;
    bool     b_suspended;
};

/* every action the default, and nothing pending */
void signal_reset(struct signal_state *s);

/* nothing blocked or pending, and no alternate stack */
void signal_reset_thread(struct signal_thread *t);

/*
 * what a process inherits across fork: the actions, but *not* the
 * pending set. a child is not born owing its parent's interrupts
 */
void signal_inherit(struct signal_state *p_child,
                    const struct signal_state *p_parent);

/*
 * and what a thread inherits from the one that made it: the mask, and
 * with b_altstack the alternate stack, which a fork's child keeps and a
 * thread sharing its maker's memory does not, the stack being its
 * maker's. nothing pending
 */
void signal_inherit_thread(struct signal_thread *p_child,
                           const struct signal_thread *p_parent,
                           bool b_altstack);

/*
 * and across exec: handlers go back to default, because the code they
 * pointed at is gone, and so does the thread's alternate stack. an
 * ignored signal stays ignored, which is how nohup works
 */
void signal_exec(struct signal_state *s, struct signal_thread *t);

/*
 * raised into p_set, from origin, or from the kernel when it is NULL,
 * for a thread that has u_blocked blocked. one the thread would ignore
 * is gone as it is raised. false for a number that is no signal
 */
bool signal_raise(const struct signal_state *s, struct signal_set *p_set,
                  uint64_t u_blocked, int i_sig,
                  const struct signal_origin *origin);

/*
 * the signal t takes next: the lowest it was sent itself and has not
 * blocked, then the lowest raised at its process. 0 for none
 */
int signal_next(const struct signal_state *s, const struct signal_thread *t);

enum signal_action signal_action_for(const struct signal_state *s, int i_sig);

/*
 * take a pending signal off t's own set, or its process's when it is not
 * in t's. for a handler: its action, the mask the handler interrupts,
 * which its frame keeps for sigreturn, and the handler's own mask put in
 * place, its signal in it unless SA_NODEFER, and the action back to
 * default when SA_RESETHAND said so
 */
enum signal_action signal_take(struct signal_state *s,
                               struct signal_thread *t, int i_sig,
                               struct sigaction *act_out,
                               uint64_t *old_mask_out,
                               struct signal_origin *origin_out);

/*
 * set an action. false when it may not be: SIGKILL and SIGSTOP take no
 * handler and cannot be ignored, a refusal the program should see
 * rather than a lie it should believe. one ignored from now on is gone
 * from the process's set; the caller takes it out of each thread's own
 */
bool signal_set_action(struct signal_state *s, int i_sig,
                       const struct sigaction *act);

/* a signal taken out of a set without being delivered */
void signal_forget(struct signal_set *p_set, int i_sig);

/*
 * block or unblock. SIGKILL and SIGSTOP silently stay unblocked, the
 * rule is not negotiable and refusing the whole call would break
 * programs that block everything as a matter of course
 */
void signal_set_mask(struct signal_thread *t, uint64_t mask);
uint64_t signal_get_mask(const struct signal_thread *t);

/*
 * what a handler with SA_SIGINFO, or sigtimedwait, is told about a
 * signal: who sent it and why, a child's news, or where a fault was
 */
void signal_info(int i_sig, const struct signal_origin *p_origin,
                 struct siginfo *p_info);

/* is there anything t could be delivered at all? */
bool signal_deliverable(const struct signal_state *s,
                        const struct signal_thread *t);

const char *signal_name(int i_sig);

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/signal.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * which signal is which, as a number a program can send.
 */

#ifndef UAPI_VELVET_SIGNAL_H
#define UAPI_VELVET_SIGNAL_H

#define SIGHUP     1
#define SIGINT     2    /* ctrl+c */
#define SIGQUIT    3
#define SIGILL     4
#define SIGTRAP    5
#define SIGABRT    6
#define SIGIOT     SIGABRT
#define SIGBUS     7    /* an access the bus refused, misaligned */
#define SIGFPE     8
#define SIGKILL    9    /* cannot be caught, blocked or ignored */
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13    /* wrote to a pipe nobody is reading */
#define SIGALRM   14
#define SIGTERM   15    /* the polite "please stop" */
#define SIGSTKFLT 16
#define SIGCHLD   17    /* a child finished. ignored by default */
#define SIGCONT   18
#define SIGSTOP   19    /* cannot be caught, blocked or ignored */
#define SIGTSTP   20    /* ctrl+z */
#define SIGTTIN   21    /* a background job read its terminal */
#define SIGTTOU   22    /* or wrote to it, when told not to */
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28    /* the terminal changed size */
#define SIGIO     29
#define SIGPOLL   SIGIO
#define SIGPWR    30
#define SIGSYS    31

/*
 * the rest, 32 to 64, carry no meaning of their own: a program, or its
 * libc, gives them one
 */
#define SIGRTMIN  32
#define SIGRTMAX  64

/* one more than the last, so that a table indexed by signal fits them all */
#define SIGNAL_MAX 65

#define SIG_DEFAULT 0
#define SIG_IGNORE  1

#endif

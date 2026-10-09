// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/signal.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * which signal is which, as a number a program can send.
 */

#ifndef UAPI_VELVET_SIGNAL_H
#define UAPI_VELVET_SIGNAL_H

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

#endif

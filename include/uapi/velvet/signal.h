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

#include <stdint.h>

/* rt_sigaction's flags */
#define SA_NOCLDSTOP 0x00000001     /* no SIGCHLD for a child that stops */
#define SA_NOCLDWAIT 0x00000002     /* children are not waited for */
#define SA_SIGINFO   0x00000004     /* the handler takes three arguments */
#define SA_RESTORER  0x04000000     /* sa_restorer is where it returns to */
#define SA_ONSTACK   0x08000000     /* on the alternate stack */
#define SA_RESTART   0x10000000     /* a call it interrupts starts again */
#define SA_NODEFER   0x40000000     /* its own signal is not blocked in it */
#define SA_RESETHAND 0x80000000     /* back to the default once run */

/* rt_sigprocmask's how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* the set, as the calls pass it: signal n at bit n - 1 */
#define SIGSET_BYTES 8

struct sigaction {
    uint64_t sa_handler;        /* SIG_DEFAULT, SIG_IGNORE, or an address */
    uint64_t sa_flags;
    uint64_t sa_restorer;
    uint64_t sa_mask;           /* blocked while the handler runs */
};

/* the alternate stack */
#define SS_ONSTACK  1
#define SS_DISABLE  2
#define MINSIGSTKSZ 2048
#define SIGSTKSZ    8192

typedef struct {
    uint64_t ss_sp;
    int32_t  ss_flags;
    int32_t  pad;
    uint64_t ss_size;
} stack_t;

/* who sent it, and why: si_code */
#define SI_USER   0
#define SI_KERNEL 0x80
#define SI_TKILL  (-6)

#define CLD_EXITED    1
#define CLD_KILLED    2
#define CLD_DUMPED    3
#define CLD_STOPPED   5
#define CLD_CONTINUED 6

#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define ILL_ILLOPC  1
#define ILL_ILLOPN  2
#define ILL_PRVOPC  5
#define FPE_INTDIV  1
#define FPE_FLTDIV  3
#define FPE_FLTINV  7
#define BUS_ADRALN  1
#define TRAP_BRKPT  1
#define TRAP_TRACE  2

/* 128 bytes, the parts of it a signal here fills in */
struct siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad;
    union {
        struct {
            int32_t  si_pid;
            uint32_t si_uid;
            int32_t  si_status;
            int32_t  pad;
            int64_t  si_utime;
            int64_t  si_stime;
        } proc;
        uint64_t si_addr;
        uint8_t  bytes[112];
    } u;
};

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/resource.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * limits, and what a process has used.
 */

#ifndef UAPI_VELVET_RESOURCE_H
#define UAPI_VELVET_RESOURCE_H

#include <stdint.h>

#define RLIMIT_CPU        0
#define RLIMIT_FSIZE      1
#define RLIMIT_DATA       2
#define RLIMIT_STACK      3
#define RLIMIT_CORE       4
#define RLIMIT_RSS        5
#define RLIMIT_NPROC      6
#define RLIMIT_NOFILE     7
#define RLIMIT_MEMLOCK    8
#define RLIMIT_AS         9
#define RLIMIT_LOCKS      10
#define RLIMIT_SIGPENDING 11
#define RLIMIT_MSGQUEUE   12
#define RLIMIT_NICE       13
#define RLIMIT_RTPRIO     14
#define RLIMIT_RTTIME     15
#define RLIM_NLIMITS      16

#define RLIM_INFINITY (~0ull)

struct rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

#define RUSAGE_SELF     0
#define RUSAGE_CHILDREN (-1)
#define RUSAGE_THREAD   1

/*
 * a time in a rusage, laid out as a timeval is, under a name of its own
 * so that including this beside a libc's time headers is no clash
 */
struct rusage_time {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct rusage {
    struct rusage_time ru_utime;
    struct rusage_time ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
};

#endif

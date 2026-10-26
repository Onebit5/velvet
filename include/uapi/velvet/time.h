// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/time.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * time as the calls about it pass it.
 */

#ifndef UAPI_VELVET_TIME_H
#define UAPI_VELVET_TIME_H

#include <stdint.h>

struct timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};

struct itimerval {
    struct timeval it_interval;
    struct timeval it_value;
};

struct tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};

#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4
#define CLOCK_REALTIME_COARSE    5
#define CLOCK_MONOTONIC_COARSE   6
#define CLOCK_BOOTTIME           7

#define TIMER_ABSTIME 1

#define ITIMER_REAL    0
#define ITIMER_VIRTUAL 1
#define ITIMER_PROF    2

#endif

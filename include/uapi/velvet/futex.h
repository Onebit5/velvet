// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/futex.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a word in a program's memory, and the calls for when it says wait.
 */

#ifndef UAPI_VELVET_FUTEX_H
#define UAPI_VELVET_FUTEX_H

#define FUTEX_WAIT              0
#define FUTEX_WAKE              1
#define FUTEX_FD                2
#define FUTEX_REQUEUE           3
#define FUTEX_CMP_REQUEUE       4
#define FUTEX_WAKE_OP           5
#define FUTEX_LOCK_PI           6
#define FUTEX_UNLOCK_PI         7
#define FUTEX_TRYLOCK_PI        8
#define FUTEX_WAIT_BITSET       9
#define FUTEX_WAKE_BITSET       10

/* the word is no other process's, which here is every word */
#define FUTEX_PRIVATE_FLAG      128
/* a WAIT_BITSET's deadline is on the wall clock rather than the uptime */
#define FUTEX_CLOCK_REALTIME    256
#define FUTEX_CMD_MASK          (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

/* what a plain WAIT and WAKE are: any waiter matches any waker */
#define FUTEX_BITSET_MATCH_ANY  0xffffffffu

#endif

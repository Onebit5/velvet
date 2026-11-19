// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/futex.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * waking whoever waits on a word, from inside the kernel.
 */

#ifndef VELVET_FUTEX_H
#define VELVET_FUTEX_H

#include <stdint.h>

/*
 * up to i_count of the threads waiting on the word at u_addr in the
 * running thread's memory woken, as a thread's end wakes whoever waits
 * for it on the word it clears: how many were
 */
int64_t futex_wake(uint64_t u_addr, int i_count);

/*
 * the robust mutexes the running thread holds, as it ends or runs
 * another program: each marked as its owner gone, with one waiter woken
 * to take it and be told, and the list forgotten
 */
void futex_robust_release(void);

#endif

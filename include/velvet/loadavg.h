// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/loadavg.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the load average, as arithmetic.
 */

/* the design notes for loadavg.h are in docs/subsystems/lib.rst */

#ifndef VELVET_LOADAVG_H
#define VELVET_LOADAVG_H

#include <stddef.h>
#include <stdint.h>

/*
 * the load is how many threads want a cpu, averaged over one minute,
 * five and fifteen: three averages, each in fixed point with this many
 * bits of fraction, a whole thread being LOAD_ONE
 */
#define LOAD_SHIFT 11
#define LOAD_ONE   ((uint64_t)1 << LOAD_SHIFT)
#define LOAD_SPANS 3

/* how often a count is folded in, in seconds */
#define LOAD_EVERY_S 5

/*
 * what an average keeps of itself at each fold: e to the minus five
 * seconds over the span it averages, in the same fixed point
 */
uint64_t load_decay(size_t sz_span);

/*
 * an average with a count of u_active threads, in fixed point, folded in.
 * rounded up while the count is above it, so that a load that stays put
 * is reached and not only ever approached
 */
uint64_t load_fold(uint64_t u_load, uint64_t u_decay, uint64_t u_active);

/* an average as the whole threads and the hundredths it is shown with */
void load_digits(uint64_t u_load, uint64_t *p_whole, uint64_t *p_hundredths);

#endif

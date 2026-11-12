// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/loadavg.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the load average: a count folded into three decaying averages.
 */

#include <velvet/loadavg.h>

/*
 * e^(-5/60), e^(-5/300) and e^(-5/900), times LOAD_ONE: the numbers
 * every unix that has shown a load has shown it with, so the same work
 * reads as the same load here as anywhere
 */
static const uint64_t s_arr_decay[LOAD_SPANS] = { 1884, 2014, 2037 };

uint64_t load_decay(size_t sz_span)
{
    return (sz_span < LOAD_SPANS) ? s_arr_decay[sz_span] : 0;
}

uint64_t load_fold(uint64_t u_load, uint64_t u_decay, uint64_t u_active)
{
    uint64_t u_new = u_load * u_decay + u_active * (LOAD_ONE - u_decay);
    if (u_active >= u_load) {
        u_new += LOAD_ONE - 1;
    }
    return u_new / LOAD_ONE;
}

void load_digits(uint64_t u_load, uint64_t *p_whole, uint64_t *p_hundredths)
{
    /* half a hundredth, so the digits are rounded rather than cut */
    uint64_t u_shown = u_load + LOAD_ONE / 200;
    *p_whole = u_shown >> LOAD_SHIFT;
    *p_hundredths = ((u_shown & (LOAD_ONE - 1)) * 100) >> LOAD_SHIFT;
}

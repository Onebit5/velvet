// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm/pressure.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what happens when memory runs out.
 */

#include "mm/pressure.h"

bool pressure_allow(uint64_t free_pages, size_t want, enum pressure_who who)
{
    if (want == 0) {
        return true;
    }
    if (want > free_pages) {
        return false;       /* more than exists, whoever is asking */
    }
    if (who == PRESSURE_KERNEL) {
        return true;
    }

    /* the reserve is what would be *left*, not what is there now. */
    return free_pages - want >= PRESSURE_RESERVE;
}

uint64_t pressure_available(uint64_t free_pages)
{
    return (free_pages > PRESSURE_RESERVE) ? free_pages - PRESSURE_RESERVE : 0;
}

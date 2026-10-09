// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/pressure.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what happens when the machine runs out.
 */

/* the design notes for pressure.h are in docs/subsystems/mm.rst */

#ifndef VELVET_PRESSURE_H
#define VELVET_PRESSURE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* what happens when the machine runs out. */

#define PRESSURE_RESERVE 256

enum pressure_who {
    PRESSURE_KERNEL,    /* may take everything, including the reserve */
    PRESSURE_USER       /* may take everything above it */
};

bool pressure_allow(uint64_t free_pages, size_t want, enum pressure_who who);

uint64_t pressure_available(uint64_t free_pages);

#endif

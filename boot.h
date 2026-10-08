// SPDX-License-Identifier: GPL-2.0-only
/*
 * boot.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what philemon left behind. one struct, handed over in rdi the way any
 * function is called, describing everything about this machine that could
 * only be found out before long mode started.
 */

/* the design notes for boot.h are in docs/subsystems/mm.rst */

#ifndef BOOT_H
#define BOOT_H

#include "philemon.h"

void boot_take_handoff(const struct ph_handoff *h);
const struct ph_handoff *boot_handoff(void);

uint64_t boot_hhdm(void);

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * init/boot.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the first steps, before there is a console to say so on.
 */

#include <velvet/boot.h>
#include <velvet/panic.h>
#include <asm/cpu.h>

static const struct boot_handoff *handoff;

void boot_take_handoff(const struct boot_handoff *h)
{
    if (h == NULL || h->magic != BOOT_MAGIC) {
        /* nothing has been set up yet, no console, no serial, so there is nowhere to complain to. */
        cpu_stop();
    }
    handoff = h;
}

const struct boot_handoff *boot_handoff(void)
{
    if (handoff == NULL) {
        panic("something asked about the boot before the boot happened");
    }
    return handoff;
}

uint64_t boot_hhdm(void)
{
    return handoff != NULL ? handoff->hhdm : BOOT_HHDM;
}

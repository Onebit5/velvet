// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/panic.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * stopping the machine loudly.
 */

#include "lib/panic.h"
#include "sched/spinlock.h"
#include "lib/kprintf.h"
#include "lib/backtrace.h"
#include "drivers/console.h"
#include "arch/irq.h"
#include "arch/cpu.h"
#include "arch/machine.h"
#include <stdarg.h>
#include <stdint.h>

void panic(const char *fmt, ...)
{
    /* whatever this core was holding, it is not going to give back. */
    spin_abandon_all();

    irq_disable();

    console_set_colors(0xe64553, 0x101018);

    kprintf("\n\n*** KERNEL PANIC ***\n\n");
    kprintf("I am thou... Thou art I...\n");
    kprintf("The bond thou hast forged hath been broken...\n\n");

    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);

    kprintf("\n\n");
    kbacktrace(0, 0);

    kprintf("\nThe Computer Arcana hath fallen to ruin.\n");
    kprintf("Yet death is not the end.\n\n");
    kprintf("Press any key to return to the Velvet Room...\n");

    /*
     * interrupts are off and never coming back, so the keyboard driver
     * is no help here, it is built entirely around an interrupt that
     * will not arrive. asking the machine directly is the only way, and
     * *how* it asks is the machine's business rather than this file's
     */
    for (;;) {
        if (machine_key_pressed()) {
            machine_reset();
        }
        cpu_relax();
    }
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/clock.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the time now: when boot was, by the battery-backed clock, and how long
 * the timer says it has been since.
 *
 * apart from epoch.c, which is arithmetic and nothing else and is
 * compiled into programs as well as into the kernel. this half asks the
 * timer, which only the kernel has
 */

#include <velvet/epoch.h>
#include <velvet/pit.h>

static uint64_t boot_epoch;

void epoch_start(uint64_t seconds_at_boot)
{
    boot_epoch = seconds_at_boot;
}

bool epoch_known(void)
{
    return boot_epoch != 0;
}

uint64_t epoch_now(void)
{
    return epoch_now_ms() / 1000;
}

uint64_t epoch_now_ms(void)
{
    if (boot_epoch == 0) {
        /* nobody has said when boot was. */
        return 0;
    }
    return boot_epoch * 1000 + pit_uptime_ms();
}

void epoch_set_ms(uint64_t u_now_ms)
{
    uint64_t u_up = pit_uptime_ms();
    boot_epoch = (u_now_ms > u_up) ? (u_now_ms - u_up) / 1000 : 1;
}

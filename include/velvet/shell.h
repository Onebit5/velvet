// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/shell.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the velvet room terminal, which is the kernel's monitor.
 */

/* the design notes for shell.h are in docs/subsystems/shell.rst */

#ifndef VELVET_SHELL_H
#define VELVET_SHELL_H

/*
 * the kernel's monitor, on VCONSOLE_MONITOR: the shell's own commands as
 * root, with nobody logged in and no programs run, for looking into the
 * kernel when something has gone wrong
 */
void shell_monitor(void);

#endif

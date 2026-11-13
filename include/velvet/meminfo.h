// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/meminfo.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what memory says about itself in /proc, laid out the way the programs
 * that show it read it.
 */

#ifndef VELVET_MEMINFO_H
#define VELVET_MEMINFO_H

/* the frames, what a program may still have, and what holds the rest */
void mm_show_meminfo(void);

/* every object cache, as slabinfo 2.1 */
void mm_show_slabinfo(void);

/*
 * /proc/meminfo and /proc/buddyinfo, anyone's, and /proc/slabinfo and
 * /proc/vmm, root's, since where the kernel keeps things is a map for
 * nobody else
 */
void mm_proc_init(void);

#endif

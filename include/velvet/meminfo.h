// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/meminfo.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what memory says about itself, to a person or in /proc.
 */

#ifndef VELVET_MEMINFO_H
#define VELVET_MEMINFO_H

/* the frames, what a program may still have, and the kernel's heap. */
void mm_show_meminfo(void);

/* every object cache, and what it holds against what it is using. */
void mm_show_slabinfo(void);

/*
 * /proc/meminfo and /proc/slabinfo, which are these two, and /proc/vmm,
 * root's, which is vmm_show
 */
void mm_proc_init(void);

#endif

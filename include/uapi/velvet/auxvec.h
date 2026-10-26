// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/auxvec.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what exec tells a program about itself, below its environment.
 */

#ifndef UAPI_VELVET_AUXVEC_H
#define UAPI_VELVET_AUXVEC_H

#define AT_NULL     0
#define AT_IGNORE   1
#define AT_PHDR     3       /* where its program headers are in memory */
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7       /* where its interpreter is, 0 for none */
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25      /* sixteen bytes nobody could predict */
#define AT_HWCAP2   26
#define AT_EXECFN   31      /* the path it was run by */

#endif

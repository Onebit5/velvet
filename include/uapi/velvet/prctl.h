// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/prctl.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * a process asking about itself, and the architecture's settings.
 */

#ifndef UAPI_VELVET_PRCTL_H
#define UAPI_VELVET_PRCTL_H

#define PR_SET_PDEATHSIG 1
#define PR_GET_PDEATHSIG 2
#define PR_SET_NAME      15
#define PR_GET_NAME      16

/* the name a process goes by, at most this long with its NUL */
#define TASK_COMM_LEN 16

/* arch_prctl, on x86_64: where %fs and %gs point */
#define ARCH_SET_GS 0x1001
#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003
#define ARCH_GET_GS 0x1004

#endif

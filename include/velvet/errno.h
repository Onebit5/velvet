// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/velvet/errno.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the errnos a program may see, and the few only the kernel ever does.
 */

#ifndef VELVET_ERRNO_H
#define VELVET_ERRNO_H

#include <uapi/velvet/errno.h>

/*
 * a call a signal interrupted, saying what should happen to it once the
 * signal has been dealt with. the way back to ring 3 turns these into a
 * restart of the call or into -EINTR, so no program ever sees one:
 *
 *   ERESTARTSYS     started again after a handler with SA_RESTART, or
 *                   when no handler ran; -EINTR after any other
 *   ERESTARTNOINTR  started again whatever happened
 *   ERESTARTNOHAND  started again only when no handler ran
 */
#define ERESTARTSYS    512
#define ERESTARTNOINTR 513
#define ERESTARTNOHAND 514

#endif

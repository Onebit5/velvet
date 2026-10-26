// SPDX-License-Identifier: GPL-2.0-only
/*
 * include/uapi/velvet/wait.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what wait4 is asked, and how it answers.
 */

#ifndef UAPI_VELVET_WAIT_H
#define UAPI_VELVET_WAIT_H

#define WNOHANG    0x00000001
#define WUNTRACED  0x00000002
#define WSTOPPED   WUNTRACED
#define WEXITED    0x00000004
#define WCONTINUED 0x00000008
#define WNOWAIT    0x01000000

/*
 * the status word: an exit code in bits 8 to 15, or the signal that
 * ended it in bits 0 to 6, or 0x7f with the signal that stopped it above,
 * or 0xffff for a process continued
 */
#define W_EXITCODE(code, sig) (((code) & 0xff) << 8 | (sig))
#define W_STOPCODE(sig)       ((sig) << 8 | 0x7f)
#define W_CONTINUED           0xffff

#endif

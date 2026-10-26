// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/uapi/asm/sigcontext.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a signal handler is handed about what it interrupted, on x86_64.
 */

#ifndef ARCH_X86_64_UAPI_ASM_SIGCONTEXT_H
#define ARCH_X86_64_UAPI_ASM_SIGCONTEXT_H

#include <stdint.h>
#include <uapi/velvet/signal.h>

/*
 * the interrupted registers, as every x86_64 libc reads them out of a
 * handler's third argument, and where the fpu's registers were put
 */
struct sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp;
    uint64_t rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;
    uint64_t reserved1[8];
};

_Static_assert(sizeof(struct sigcontext) == 256,
               "a sigcontext is 256 bytes, as every libc reads it");

/* the handler's third argument: the registers and the mask to go back to */
struct ucontext {
    uint64_t uc_flags;
    uint64_t uc_link;
    stack_t uc_stack;
    struct sigcontext uc_mcontext;
    uint64_t uc_sigmask;
};

#endif

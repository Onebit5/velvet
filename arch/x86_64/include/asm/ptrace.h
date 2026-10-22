// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/ptrace.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the registers of whatever was running when the kernel was entered.
 */

#ifndef ARCH_X86_64_ASM_PTRACE_H
#define ARCH_X86_64_ASM_PTRACE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * one frame for every way into the kernel. an interrupt and an exception
 * push it in isr.asm, and a system call builds the same thing in
 * syscall.asm, so a signal, a fork or a program faulting looks at one
 * layout whichever door the thread came in by, and every way out is the
 * same `iretq`.
 *
 * `vector` is the interrupt's number, or for a system call the call's
 * own number, kept apart from rax because rax is about to become the
 * answer and a restarted call needs the question back
 */
struct pt_regs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error_code;
    /* pushed by the cpu itself, or by syscall.asm in its place */
    uint64_t rip, cs, rflags, rsp, ss;
};

_Static_assert(offsetof(struct pt_regs, vector) == 15 * 8,
               "pt_regs drifted from the push order in isr.asm");
_Static_assert(sizeof(struct pt_regs) == 22 * 8,
               "pt_regs drifted from the push order in isr.asm");

/* the frame was pushed on the way in from ring 3 */
static inline bool user_mode(const struct pt_regs *regs)
{
    return (regs->cs & 3) == 3;
}

#endif

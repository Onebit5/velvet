// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/syscall.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the door between ring 3 and here.
 */

#ifndef ARCH_X86_64_ASM_SYSCALL_H
#define ARCH_X86_64_ASM_SYSCALL_H

#include <stdint.h>

/* the numbers, and the structs ring 3 reads, are the abi */
#include <uapi/velvet/syscall.h>

#include <asm/ptrace.h>

/*
 * leave for ring 3 through a frame rather than through a call. the only
 * caller is a forked child, whose frame already holds the zero in rax
 * that is how it finds out it is the child
 */
void fork_return(struct pt_regs *frame) __attribute__((noreturn));

/* called by the entry stub with the frame it built, which it answers in */
void syscall_from_user(struct pt_regs *regs);

/*
 * everything that has to happen on the way back to ring 3, from a call,
 * an interrupt or an exception alike: a pending signal is delivered,
 * run in a handler or acted on. a program busy in a loop makes no calls
 * to be signalled on the way out of, and SIGKILL has to reach it all the
 * same
 */
void exit_to_user(struct pt_regs *regs);

/* wire up STAR/LSTAR/SFMASK and turn on EFER.SCE */
void syscall_init(void);

/*
 * which kernel stack `syscall` should land on. the scheduler keeps this
 * pointed at the running thread, exactly like the tss rsp0
 */
/*
 * where this core's kernel stack is, for the entry stub to stand on.
 * per core, not global: the scheduler on one core must not be able to
 * rewrite the stack another core is about to use
 */
void syscall_set_kernel_rsp(uint64_t rsp);

#endif

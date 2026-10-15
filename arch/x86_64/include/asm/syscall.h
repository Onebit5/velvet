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

/*
 * everything ring 3 was holding when it made the call, written down by
 * the entry stub in the order it pushes them.
 *
 * this exists for fork and for nothing else. a forked child has to come
 * back from a syscall it never made, holding exactly what its parent
 * held, and the callee-saved half of that is in the cpu at the moment
 * of the call and gone a moment later, buried under some C prologue.
 *
 * the layout is the stack layout. changing either without the other is
 * a program that resumes with its registers shuffled, which is the kind
 * of bug that looks like the compiler being wrong
 */
struct user_regs {
    uint64_t r15, r14, r13, r12, rbx, rbp;
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t r11;       /* the user's rflags, courtesy of syscall */
    uint64_t rcx;       /* the user's rip, likewise */
    uint64_t rsp;
};

/*
 * fifteen registers, fifteen pushes. this catches a field appearing or
 * going away and cannot catch a reordering, for that the only real
 * check is reading the two exit paths in
 * `objdump -d bin/velvet --disassemble=syscall_entry` and
 * `--disassemble=fork_return` and seeing the same order twice
 */
_Static_assert(sizeof(struct user_regs) == 15 * 8,
               "user_regs and the pushes in syscall.asm have drifted apart");

/*
 * leave for ring 3 through a frame rather than through a call. rax is
 * zeroed on the way out, because the only caller is a forked child and
 * that is how it finds out it is the child
 */
void fork_return(struct user_regs *frame);

/*
 * for an interrupt about to go back to ring 3: a pending signal that
 * ends the process ends it here. a program busy in a loop makes no
 * calls for one to be delivered on the way out of, and SIGKILL has to
 * end it all the same
 */
void syscall_end_if_fatal(void);


/* wire up STAR/LSTAR/SFMASK and turn on EFER.SCE */
void syscall_init(void);

/*
 * how many times each has been asked for, and what to call it. the
 * numbers are the cheapest possible picture of what a program actually
 * does, one line of arithmetic per call, and afterwards you can say
 * with certainty which door gets used
 */
uint64_t syscall_times_called(unsigned nr);
const char *syscall_name(unsigned nr);

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

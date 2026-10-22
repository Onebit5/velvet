// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/fpu.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the x87 and sse registers, of which every thread has a set.
 */

#ifndef ARCH_X86_64_ASM_FPU_H
#define ARCH_X86_64_ASM_FPU_H

#include <stdint.h>

#include <velvet/compiler.h>

/*
 * what fxsave writes and fxrstor reads: the x87 stack with its control
 * and status words, mxcsr, and the sixteen xmm registers. 512 bytes on a
 * 16 byte boundary, or either instruction faults
 */
struct fpu_state {
    uint16_t u_fcw;             /* x87 control word */
    uint16_t u_fsw;             /* x87 status word */
    uint8_t  u_ftw;             /* which of st0..st7 hold something */
    uint8_t  u_reserved;
    uint16_t u_fop;
    uint64_t u_fip;
    uint64_t u_fdp;
    uint32_t u_mxcsr;           /* sse control and status */
    uint32_t u_mxcsr_mask;      /* which mxcsr bits this cpu has */
    uint8_t  arr_st[128];       /* st0..st7, 16 bytes apiece */
    uint8_t  arr_xmm[256];      /* xmm0..xmm15 */
    uint8_t  arr_unused[96];
} __attribute__((aligned(16)));

_Static_assert(sizeof(struct fpu_state) == 512,
               "fxsave writes exactly 512 bytes");

/*
 * what a program starts with: every exception masked, double extended
 * precision, round to nearest. the numbers every x86_64 program expects
 * to find, since its abi says so
 */
#define FPU_FCW_DEFAULT   0x037f
#define FPU_MXCSR_DEFAULT 0x1f80

/*
 * the mxcsr bits a cpu without a mask of its own has. fxrstor faults on
 * any other, so an image a program hands back has them cleared first
 */
#define FPU_MXCSR_MASK_DEFAULT 0xffbf

/* turn the fpu on for this core: once on each, before any thread runs */
void fpu_init_cpu(void);

/* the state a thread starts with, and a program after exec */
void fpu_init_state(struct fpu_state *p_state);

/*
 * make an image from ring 3 one fxrstor accepts: mxcsr bits this cpu
 * lacks are cleared rather than faulted on
 */
void fpu_sanitize(struct fpu_state *p_state);

#ifdef VELVET_HOSTED

ARCH_INLINE void fpu_save(struct fpu_state *p_state)
{
    (void)p_state;
}
ARCH_INLINE void fpu_restore(const struct fpu_state *p_state)
{
    (void)p_state;
}

#else

/*
 * the registers into memory and back. nothing in the kernel touches
 * them, it is built without the fpu, so what the registers hold is
 * always the running thread's
 */
ARCH_INLINE void fpu_save(struct fpu_state *p_state)
{
    __asm__ volatile ("fxsave64 %0" : "=m"(*p_state));
}

ARCH_INLINE void fpu_restore(const struct fpu_state *p_state)
{
    __asm__ volatile ("fxrstor64 %0" : : "m"(*p_state));
}

#endif

#endif

// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/fpu.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the x87 and sse registers: turned on, and handed to each thread.
 */

#include <asm/fpu.h>
#include <velvet/string.h>

#define CR0_MP (1ull << 1)      /* wait honours TS, as fwait should */
#define CR0_EM (1ull << 2)      /* every fpu instruction is #NM */
#define CR0_TS (1ull << 3)      /* the next fpu instruction is #NM */
#define CR0_NE (1ull << 5)      /* x87 errors are #MF, not an irq */

#define CR4_OSFXSR     (1ull << 9)      /* fxsave, fxrstor, and sse at all */
#define CR4_OSXMMEXCPT (1ull << 10)     /* sse errors are #XM, not #UD */

/*
 * which mxcsr bits this cpu has, from its own fxsave. 0 there means the
 * cpu predates the field, and has the bits every one of them has
 */
static uint32_t s_u_mxcsr_mask = FPU_MXCSR_MASK_DEFAULT;

/*
 * the registers are saved and restored on every switch rather than when
 * a thread first touches them. a trap on first use saves a few hundred
 * cycles on threads that never do, and costs an #NM path, a per core
 * owner and a migration rule to get wrong, for a kernel whose programs
 * are built for sse anyway
 */
void fpu_init_cpu(void)
{
    uint64_t u_cr0;
    uint64_t u_cr4;
    uint32_t u_mxcsr = FPU_MXCSR_DEFAULT;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(u_cr0));
    u_cr0 &= ~(CR0_EM | CR0_TS);
    u_cr0 |= CR0_MP | CR0_NE;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(u_cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(u_cr4));
    u_cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(u_cr4));

    __asm__ volatile ("fninit");
    __asm__ volatile ("ldmxcsr %0" : : "m"(u_mxcsr));

    struct fpu_state probe;
    fpu_save(&probe);
    if (probe.u_mxcsr_mask != 0) {
        s_u_mxcsr_mask = probe.u_mxcsr_mask;
    }
}

void fpu_init_state(struct fpu_state *p_state)
{
    memset(p_state, 0, sizeof *p_state);
    p_state->u_fcw = FPU_FCW_DEFAULT;
    p_state->u_mxcsr = FPU_MXCSR_DEFAULT;
}

void fpu_sanitize(struct fpu_state *p_state)
{
    p_state->u_mxcsr &= s_u_mxcsr_mask;
}

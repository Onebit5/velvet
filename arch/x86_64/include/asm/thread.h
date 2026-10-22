// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/thread.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what a thread holds on x86_64 beyond what the switch saves.
 */

#ifndef ARCH_X86_64_ASM_THREAD_H
#define ARCH_X86_64_ASM_THREAD_H

#include <stdint.h>

#include <velvet/compiler.h>
#include <asm/fpu.h>
#include <asm/msr.h>

/*
 * the registers switch_context does not carry, kept in the thread while
 * it is off the cpu: the fpu's, and where %fs points in ring 3. both
 * are the program's, which sets them and expects them back
 */
struct thread_arch {
    struct fpu_state fpu;
    uint64_t u_fs_base;
};

#ifdef VELVET_HOSTED

ARCH_INLINE void fs_base_load(uint64_t u_base)
{
    (void)u_base;
}

#else

/* where %fs points from here on, on this core */
ARCH_INLINE void fs_base_load(uint64_t u_base)
{
    wrmsr(MSR_FS_BASE, u_base);
}

#endif

#endif

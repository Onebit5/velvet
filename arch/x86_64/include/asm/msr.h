// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/msr.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * model-specific registers, and the gs base.
 */

#ifndef ARCH_X86_64_ASM_MSR_H
#define ARCH_X86_64_ASM_MSR_H

#include <stdint.h>

/* model specific registers and cpuid. only a couple of each so far */

#define MSR_EFER      0xc0000080
#define EFER_NXE      (1ull << 11)   /* honour the no-execute bit */

/*
 * where gs points. the pair exists so a kernel can keep one base for
 * ring 3 and another for ring 0 and swap between them, this one keeps
 * the same value in both, so gs simply names the current core wherever
 * it is read from. see the note at the top of syscall.asm for why
 */
#define MSR_GS_BASE        0xc0000101
#define MSR_KERNEL_GS_BASE 0xc0000102

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    asm volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    asm volatile ("wrmsr"
                  :
                  : "c"(msr), "a"((uint32_t)value), "d"((uint32_t)(value >> 32)));
}

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx)
{
    asm volatile ("cpuid"
                  : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                  : "a"(leaf), "c"(0));
}

#endif

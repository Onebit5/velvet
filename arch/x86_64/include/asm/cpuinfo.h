// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/include/asm/cpuinfo.h
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * what the cpu says about itself when asked nicely
 */

#ifndef ARCH_X86_64_ASM_CPUINFO_H
#define ARCH_X86_64_ASM_CPUINFO_H

#include <stdint.h>

/* the 12-character vendor string, e.g. "GenuineIntel". buf needs 13 */
void cpu_vendor(char *buf);

/*
 * the 48-character marketing name, e.g. "AMD Ryzen 9 ...". buf needs 49.
 * older or emulated cpus may not have one, in which case you get the
 * vendor string instead
 */
void cpu_brand(char *buf);

/* where in cpuid a feature's bit is: a leaf, and a register of it */
enum cpuid_word {
    CPUID_1_EDX,                /* leaf 1 */
    CPUID_EXT_EDX,              /* leaf 0x80000001 */
    CPUID_1_ECX,
    CPUID_EXT_ECX,
    CPUID_7_EBX,                /* leaf 7, the first of its subleaves */
    CPUID_7_ECX,
    CPUID_7_EDX,
    CPUID_WORDS,
};

/* what every core of the machine says alike */
struct cpu_ident {
    char arr_vendor[13];
    char arr_brand[49];
    uint32_t u_family;
    uint32_t u_model;
    uint32_t u_stepping;
    uint32_t u_level;           /* the highest basic leaf */
    uint32_t u_clflush;         /* the bytes a cache line flush covers */
    uint32_t u_phys_bits;
    uint32_t u_virt_bits;
    uint32_t arr_words[CPUID_WORDS];
};

/* this core's, asked of cpuid */
void cpu_identify(struct cpu_ident *p_out);

/*
 * one core's block of /proc/cpuinfo: core u_cpu of u_cores, its local
 * apic numbered u_apic, and what every core says alike
 */
void cpu_show_core(const struct cpu_ident *p_id, uint32_t u_cpu,
                   uint32_t u_cores, uint32_t u_apic);

#endif

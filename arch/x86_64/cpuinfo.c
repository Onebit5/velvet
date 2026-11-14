// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/cpuinfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * cpuid: the vendor, the brand, and the feature bits, and a core's block
 * of /proc/cpuinfo made of them.
 */

#include <asm/cpuinfo.h>
#include <asm/msr.h>
#include <velvet/kprintf.h>

void cpu_vendor(char *buf)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0, &eax, &ebx, &ecx, &edx);

    /*
     * the vendor arrives split across three registers in an order that
     * only makes sense if you picture it as ebx:edx:ecx
     */
    uint32_t words[3] = { ebx, edx, ecx };
    for (int i = 0; i < 3; i++) {
        for (int b = 0; b < 4; b++) {
            buf[i * 4 + b] = (char)((words[i] >> (b * 8)) & 0xff);
        }
    }
    buf[12] = '\0';
}

void cpu_brand(char *buf)
{
    uint32_t eax, ebx, ecx, edx;

    /*
     * leaf 0x80000000 tells the kernel how far the extended leaves go. the
     * brand string lives in three of them and not every cpu has it
     */
    cpuid(0x80000000, &eax, &ebx, &ecx, &edx);
    if (eax < 0x80000004) {
        cpu_vendor(buf);
        return;
    }

    int at = 0;
    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        cpuid(leaf, &eax, &ebx, &ecx, &edx);
        uint32_t words[4] = { eax, ebx, ecx, edx };
        for (int i = 0; i < 4; i++) {
            for (int b = 0; b < 4; b++) {
                buf[at++] = (char)((words[i] >> (b * 8)) & 0xff);
            }
        }
    }
    buf[48] = '\0';

    /* the string is padded with spaces more often than not, either end */
    int lead = 0;
    while (buf[lead] == ' ') {
        lead++;
    }
    if (lead > 0) {
        int i = 0;
        while (buf[lead + i] != '\0') { buf[i] = buf[lead + i]; i++; }
        buf[i] = '\0';
    }
    int i_end = 0;
    while (buf[i_end] != '\0') {
        i_end++;
    }
    while (i_end > 0 && buf[i_end - 1] == ' ') {
        buf[--i_end] = '\0';
    }
}

struct cpu_flag {
    uint8_t u_word;
    uint8_t u_bit;
    const char *str_name;
};

/*
 * what /proc/cpuinfo calls each feature, in the order and by the names
 * the programs that look for one know them by. a feature a program can
 * only use once the kernel has turned something on for it, and this
 * kernel has not, is left out, since a program that read it here would
 * use it and fault: avx and everything built on it, the avx-512 and amx
 * families, fma, f16c, xop, vaes and vpclmulqdq need the xsave state
 * this kernel does not enable, and so does xsave itself; fsgsbase, pku
 * and ospke need cr4 bits it leaves clear, vmx, svm and smx the bits a
 * kernel that ran machines of its own would set, sgx and enqcmd setup
 * it does not do. smep, smap, umip, la57 and ibt say what a kernel
 * protects itself with, and this one does not use them
 */
static const struct cpu_flag s_arr_flags[] = {
    { CPUID_1_EDX, 0, "fpu" }, { CPUID_1_EDX, 1, "vme" },
    { CPUID_1_EDX, 2, "de" }, { CPUID_1_EDX, 3, "pse" },
    { CPUID_1_EDX, 4, "tsc" }, { CPUID_1_EDX, 5, "msr" },
    { CPUID_1_EDX, 6, "pae" }, { CPUID_1_EDX, 7, "mce" },
    { CPUID_1_EDX, 8, "cx8" }, { CPUID_1_EDX, 9, "apic" },
    { CPUID_1_EDX, 11, "sep" }, { CPUID_1_EDX, 12, "mtrr" },
    { CPUID_1_EDX, 13, "pge" }, { CPUID_1_EDX, 14, "mca" },
    { CPUID_1_EDX, 15, "cmov" }, { CPUID_1_EDX, 16, "pat" },
    { CPUID_1_EDX, 17, "pse36" }, { CPUID_1_EDX, 18, "pn" },
    { CPUID_1_EDX, 19, "clflush" }, { CPUID_1_EDX, 21, "dts" },
    { CPUID_1_EDX, 22, "acpi" }, { CPUID_1_EDX, 23, "mmx" },
    { CPUID_1_EDX, 24, "fxsr" }, { CPUID_1_EDX, 25, "sse" },
    { CPUID_1_EDX, 26, "sse2" }, { CPUID_1_EDX, 27, "ss" },
    { CPUID_1_EDX, 28, "ht" }, { CPUID_1_EDX, 29, "tm" },
    { CPUID_1_EDX, 30, "ia64" }, { CPUID_1_EDX, 31, "pbe" },

    { CPUID_EXT_EDX, 11, "syscall" }, { CPUID_EXT_EDX, 19, "mp" },
    { CPUID_EXT_EDX, 20, "nx" }, { CPUID_EXT_EDX, 22, "mmxext" },
    { CPUID_EXT_EDX, 25, "fxsr_opt" }, { CPUID_EXT_EDX, 26, "pdpe1gb" },
    { CPUID_EXT_EDX, 27, "rdtscp" }, { CPUID_EXT_EDX, 29, "lm" },
    { CPUID_EXT_EDX, 30, "3dnowext" }, { CPUID_EXT_EDX, 31, "3dnow" },

    { CPUID_1_ECX, 0, "pni" }, { CPUID_1_ECX, 1, "pclmulqdq" },
    { CPUID_1_ECX, 2, "dtes64" }, { CPUID_1_ECX, 3, "monitor" },
    { CPUID_1_ECX, 4, "ds_cpl" }, { CPUID_1_ECX, 7, "est" },
    { CPUID_1_ECX, 8, "tm2" }, { CPUID_1_ECX, 9, "ssse3" },
    { CPUID_1_ECX, 10, "cid" }, { CPUID_1_ECX, 11, "sdbg" },
    { CPUID_1_ECX, 13, "cx16" }, { CPUID_1_ECX, 14, "xtpr" },
    { CPUID_1_ECX, 15, "pdcm" }, { CPUID_1_ECX, 17, "pcid" },
    { CPUID_1_ECX, 18, "dca" }, { CPUID_1_ECX, 19, "sse4_1" },
    { CPUID_1_ECX, 20, "sse4_2" }, { CPUID_1_ECX, 21, "x2apic" },
    { CPUID_1_ECX, 22, "movbe" }, { CPUID_1_ECX, 23, "popcnt" },
    { CPUID_1_ECX, 24, "tsc_deadline_timer" }, { CPUID_1_ECX, 25, "aes" },
    { CPUID_1_ECX, 30, "rdrand" }, { CPUID_1_ECX, 31, "hypervisor" },

    { CPUID_EXT_ECX, 0, "lahf_lm" }, { CPUID_EXT_ECX, 1, "cmp_legacy" },
    { CPUID_EXT_ECX, 3, "extapic" },
    { CPUID_EXT_ECX, 4, "cr8_legacy" }, { CPUID_EXT_ECX, 5, "abm" },
    { CPUID_EXT_ECX, 6, "sse4a" }, { CPUID_EXT_ECX, 7, "misalignsse" },
    { CPUID_EXT_ECX, 8, "3dnowprefetch" }, { CPUID_EXT_ECX, 9, "osvw" },
    { CPUID_EXT_ECX, 10, "ibs" }, { CPUID_EXT_ECX, 12, "skinit" },
    { CPUID_EXT_ECX, 13, "wdt" }, { CPUID_EXT_ECX, 17, "tce" },
    { CPUID_EXT_ECX, 19, "nodeid_msr" }, { CPUID_EXT_ECX, 21, "tbm" },
    { CPUID_EXT_ECX, 22, "topoext" }, { CPUID_EXT_ECX, 23, "perfctr_core" },
    { CPUID_EXT_ECX, 24, "perfctr_nb" }, { CPUID_EXT_ECX, 26, "bpext" },
    { CPUID_EXT_ECX, 27, "ptsc" }, { CPUID_EXT_ECX, 28, "perfctr_llc" },
    { CPUID_EXT_ECX, 29, "mwaitx" },

    { CPUID_7_EBX, 1, "tsc_adjust" }, { CPUID_7_EBX, 3, "bmi1" },
    { CPUID_7_EBX, 4, "hle" }, { CPUID_7_EBX, 8, "bmi2" },
    { CPUID_7_EBX, 9, "erms" }, { CPUID_7_EBX, 10, "invpcid" },
    { CPUID_7_EBX, 11, "rtm" }, { CPUID_7_EBX, 12, "cqm" },
    { CPUID_7_EBX, 15, "rdt_a" }, { CPUID_7_EBX, 18, "rdseed" },
    { CPUID_7_EBX, 19, "adx" }, { CPUID_7_EBX, 23, "clflushopt" },
    { CPUID_7_EBX, 24, "clwb" }, { CPUID_7_EBX, 25, "intel_pt" },
    { CPUID_7_EBX, 29, "sha_ni" },

    { CPUID_7_ECX, 5, "waitpkg" }, { CPUID_7_ECX, 8, "gfni" },
    { CPUID_7_ECX, 13, "tme" }, { CPUID_7_ECX, 22, "rdpid" },
    { CPUID_7_ECX, 24, "bus_lock_detect" }, { CPUID_7_ECX, 25, "cldemote" },
    { CPUID_7_ECX, 27, "movdiri" }, { CPUID_7_ECX, 28, "movdir64b" },

    { CPUID_7_EDX, 4, "fsrm" }, { CPUID_7_EDX, 10, "md_clear" },
    { CPUID_7_EDX, 14, "serialize" }, { CPUID_7_EDX, 16, "tsxldtrk" },
    { CPUID_7_EDX, 18, "pconfig" }, { CPUID_7_EDX, 28, "flush_l1d" },
    { CPUID_7_EDX, 29, "arch_capabilities" },
    { CPUID_7_EDX, 31, "spec_ctrl_ssbd" },
};

void cpu_identify(struct cpu_ident *p_out)
{
    uint32_t u_eax, u_ebx, u_ecx, u_edx;

    cpu_vendor(p_out->arr_vendor);
    cpu_brand(p_out->arr_brand);
    for (int i_at = 0; i_at < CPUID_WORDS; i_at++) {
        p_out->arr_words[i_at] = 0;
    }

    cpuid(0, &u_eax, &u_ebx, &u_ecx, &u_edx);
    p_out->u_level = u_eax;

    /*
     * family, model and stepping are packed into leaf 1's eax, with an
     * extended family added to fifteen and an extended model put above
     * the model for family six and later, as the vendors number them
     */
    cpuid(1, &u_eax, &u_ebx, &u_ecx, &u_edx);
    p_out->u_stepping = u_eax & 0xf;
    p_out->u_family = (u_eax >> 8) & 0xf;
    if (p_out->u_family == 0xf) {
        p_out->u_family += (u_eax >> 20) & 0xff;
    }
    p_out->u_model = (u_eax >> 4) & 0xf;
    if (p_out->u_family >= 6) {
        p_out->u_model += ((u_eax >> 16) & 0xf) << 4;
    }
    p_out->u_clflush = ((u_ebx >> 8) & 0xff) * 8;
    p_out->arr_words[CPUID_1_EDX] = u_edx;
    p_out->arr_words[CPUID_1_ECX] = u_ecx;

    if (p_out->u_level >= 7) {
        cpuid(7, &u_eax, &u_ebx, &u_ecx, &u_edx);
        p_out->arr_words[CPUID_7_EBX] = u_ebx;
        p_out->arr_words[CPUID_7_ECX] = u_ecx;
        p_out->arr_words[CPUID_7_EDX] = u_edx;
    }

    /* the widths of an address, where the cpu says, else the long mode floor */
    uint32_t u_top;
    cpuid(0x80000000, &u_top, &u_ebx, &u_ecx, &u_edx);
    p_out->u_phys_bits = 36;
    p_out->u_virt_bits = 48;
    if (u_top >= 0x80000001) {
        cpuid(0x80000001, &u_eax, &u_ebx, &u_ecx, &u_edx);
        p_out->arr_words[CPUID_EXT_EDX] = u_edx;
        p_out->arr_words[CPUID_EXT_ECX] = u_ecx;
    }
    if (u_top >= 0x80000008) {
        cpuid(0x80000008, &u_eax, &u_ebx, &u_ecx, &u_edx);
        p_out->u_phys_bits = u_eax & 0xff;
        p_out->u_virt_bits = (u_eax >> 8) & 0xff;
    }
}

void cpu_show_core(const struct cpu_ident *p_id, uint32_t u_cpu,
                   uint32_t u_cores, uint32_t u_apic)
{
    kprintf("processor\t: %u\nvendor_id\t: %s\ncpu family\t: %u\n"
            "model\t\t: %u\nmodel name\t: %s\nstepping\t: %u\n", u_cpu,
            p_id->arr_vendor, p_id->u_family, p_id->u_model,
            p_id->arr_brand, p_id->u_stepping);

    /* every core its own, as far as this kernel knows, in one package */
    kprintf("physical id\t: 0\nsiblings\t: %u\ncore id\t\t: %u\n"
            "cpu cores\t: %u\napicid\t\t: %u\ninitial apicid\t: %u\n",
            u_cores, u_cpu, u_cores, u_apic, u_apic);
    kprintf("fpu\t\t: yes\nfpu_exception\t: yes\ncpuid level\t: %u\n"
            "wp\t\t: yes\nflags\t\t:", p_id->u_level);
    for (size_t sz_at = 0; sz_at < sizeof s_arr_flags / sizeof *s_arr_flags;
         sz_at++) {
        const struct cpu_flag *p_flag = &s_arr_flags[sz_at];
        if ((p_id->arr_words[p_flag->u_word] >> p_flag->u_bit) & 1) {
            kprintf(" %s", p_flag->str_name);
        }
    }
    kprintf("\nclflush size\t: %u\ncache_alignment\t: %u\n"
            "address sizes\t: %u bits physical, %u bits virtual\n"
            "power management:\n\n", p_id->u_clflush, p_id->u_clflush,
            p_id->u_phys_bits, p_id->u_virt_bits);
}

// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/cpuinfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * cpuid: the vendor, the brand, and the feature bits.
 */

#include "arch/x86_64/cpuinfo.h"
#include "arch/x86_64/msr.h"

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

    /* the string is padded with leading spaces more often than not */
    int lead = 0;
    while (buf[lead] == ' ') {
        lead++;
    }
    if (lead > 0) {
        int i = 0;
        while (buf[lead + i] != '\0') { buf[i] = buf[lead + i]; i++; }
        buf[i] = '\0';
    }
}

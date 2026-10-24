// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/cpuinfo.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * cpuid: the vendor, the brand, and the feature bits.
 */

#include <asm/cpuinfo.h>
#include <asm/cpu.h>
#include <asm/msr.h>
#include <asm/smp.h>
#include <velvet/kprintf.h>
#include <velvet/sched.h>

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

/*
 * the processors: what they are, which the firmware listed, which woke
 * and which are taking work. what `cpus` prints and /proc/cpuinfo is
 */
void cpu_show(void)
{
    char arr_brand[49];

    cpu_brand(arr_brand);
    kprintf("model name  %s\n\n", arr_brand);

    size_t n = smp_cpu_count();
    if (n == 0) {
        kprintf("the firmware never said how many processors this machine "
                "has,\nso only the one the kernel woke up on is being used.\n");
        return;
    }

    kprintf("%-4s %-6s %-8s %s\n", "cpu", "apic", "state", "running");
    for (size_t i = 0; i < n; i++) {
        const struct cpu *c = smp_cpu_at(i);
        const char *what = "halted";
        if (c->online && c->scheduling) {
            what = sched_cpu_running(c->index);
        } else if (c->online) {
            what = "awake, not scheduling";
        }
        kprintf("%-4u %-6u %-8s %s%s\n", c->index, c->apic_id,
                c->online ? "awake" : "silent", what,
                (c->online && !c->bootstrap && c->reported_id != c->apic_id)
                    ? "  (and reported a different apic id!)" : "");
    }

    kprintf("\n%zu of %zu processors awake, %zu taking work.\n\n",
            smp_online_count(), n, sched_cores_scheduling());
    kprintf("these are cores, not threads. /proc/threads lists threads and\n");
    kprintf("says which core each is on, a thread that is merely ready is\n");
    kprintf("on none of them. there is one run queue and every core picks\n");
    kprintf("from it, so a new thread lands wherever there is room.\n");
}

/* cpuid leaf 1, ecx: rdrand is there */
#define CPUID_RDRAND (1u << 30)

bool cpu_random(uint64_t *p_out)
{
    static int s_i_has = -1;
    if (s_i_has < 0) {
        uint32_t u_eax, u_ebx, u_ecx, u_edx;
        cpuid(1, &u_eax, &u_ebx, &u_ecx, &u_edx);
        s_i_has = (u_ecx & CPUID_RDRAND) != 0;
    }
    if (!s_i_has) {
        return false;
    }
    /*
     * it may come back empty when asked too fast, and the manual's advice
     * is a few more tries, not a loop
     */
    for (int i_try = 0; i_try < 10; i_try++) {
        uint64_t u_value;
        uint8_t u_ok;
        __asm__ volatile ("rdrand %0; setc %1"
                          : "=r"(u_value), "=qm"(u_ok) : : "cc");
        if (u_ok) {
            *p_out = u_value;
            return true;
        }
    }
    return false;
}

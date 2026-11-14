// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/cpu.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the processors, described: as /proc/cpuinfo has them for a program,
 * and as the firmware listed and woke them, for a person. and the bytes
 * a core's own random number generator has to give.
 */

#include <asm/cpu.h>
#include <asm/cpuinfo.h>
#include <asm/msr.h>
#include <asm/smp.h>
#include <velvet/kprintf.h>
#include <velvet/sched.h>

/* cpuinfo: a block for each core taking work, any thread's to run on */
void cpu_show(void)
{
    struct cpu_ident id;
    cpu_identify(&id);

    uint64_t u_cores = sched_cores_mask();
    uint32_t u_count = (uint32_t)sched_cores_scheduling();
    for (uint32_t u_cpu = 0; u_cpu < 64; u_cpu++) {
        if (((u_cores >> u_cpu) & 1) == 0) {
            continue;
        }
        const struct cpu *c = smp_cpu_at(u_cpu);
        cpu_show_core(&id, u_cpu, u_count, (c != NULL) ? c->apic_id : 0);
    }
}

/*
 * the processors as the firmware listed them, which woke, which are
 * taking work and what each is running: the end of /proc/threads
 */
void cpu_show_cores(void)
{
    size_t sz_count = smp_cpu_count();
    if (sz_count == 0) {
        kprintf("\nthe firmware never said how many processors this machine "
                "has,\nso only the one the kernel woke up on is being used.\n");
        return;
    }

    kprintf("\n%-4s %-6s %-8s %s\n", "cpu", "apic", "state", "running");
    for (size_t sz_at = 0; sz_at < sz_count; sz_at++) {
        const struct cpu *c = smp_cpu_at(sz_at);
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

    kprintf("\n%zu of %zu processors awake, %zu taking work.\n",
            smp_online_count(), sz_count, sched_cores_scheduling());
    kprintf("a thread that is merely ready is on none of them. there is one\n");
    kprintf("run queue and every core picks from it, so a new thread lands\n");
    kprintf("wherever there is room.\n");
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

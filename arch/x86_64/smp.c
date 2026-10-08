// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/smp.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * waking the other cores.
 */

#include "arch/x86_64/smp.h"
#include "arch/x86_64/lapic.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "drivers/pit.h"
#include "arch/x86_64/msr.h"
#include "arch/x86_64/tss.h"
#include "lib/kprintf.h"
#include "sched/spinlock.h"
#include "lib/string.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "sched/sched.h"
#include "arch/x86_64/syscall.h"
#include "sched/thread.h"

/*
 * the trampoline, assembled separately and carried here as bytes. it has
 * to run at a fixed low address in real mode, which is nowhere the
 * linker would ever put anything
 */
extern const uint8_t smp_trampoline[];
extern const size_t  smp_trampoline_size;

#define AP_STACK_PAGES 4

/*
 * a woken core has no idt, so any fault it takes triple faults the
 * machine and resets it with nothing said. until it has one, the only
 * way to know how far it got is for it to say so as it goes, straight
 * at the port, because kprintf takes locks that do not exist yet and
 * this core has no business holding anything
 */
static bool ap_loud;

static void ap_say(char c)
{
    if (!ap_loud) {
        return;
    }
    for (int spin = 0; spin < 100000; spin++) {
        uint8_t status;
        __asm__ volatile ("inb %1, %0" : "=a"(status) : "Nd"((uint16_t)0x3fd));
        if (status & 0x20) {
            break;
        }
    }
    __asm__ volatile ("outb %0, %1" :: "a"((uint8_t)c), "Nd"((uint16_t)0x3f8));
}

static struct cpu cpus[SMP_MAX_CPUS];
static size_t cpu_count;
static size_t online;

size_t smp_cpu_count(void)
{
    return cpu_count;
}
size_t smp_online_count(void)
{
    return online;
}

const struct cpu *smp_cpu_at(size_t index)
{
    return (index < cpu_count) ? &cpus[index] : NULL;
}

uint32_t smp_this_cpu(void)
{
    /*
     * out of the task register, which every core loaded differently.
     * this is asked on every lock and every context switch, so it has to
     * be cheap, reading the local apic would be an uncached access to
     * a device, thousands of times a second, to learn something the cpu
     * already knows
     */
    return tss_current_cpu();
}


/*
 * the first C a second processor has ever executed. it arrives on page
 * tables that are a copy of the kernel's with a low identity map added,
 * because without that the instruction after paging came on would have
 * been unreachable
 */
void smp_ap_entry(struct cpu *me);

void smp_ap_entry(struct cpu *me)
{
    ap_say('G');

    /*
     * onto the kernel's own tables. this could not be done in the
     * trampoline: the moment the identity map goes, the code doing it
     * would be unmapped mid-instruction-fetch
     */
    __asm__ volatile ("movq %0, %%cr3" :: "r"(vmm_kernel_pml4()) : "memory");
    ap_say('H');

    /*
     * the real descriptor tables. until now this core has been running
     * on the trampoline's gdt, which sits at a low address the kernel
     * does not map, so any interrupt at all would have been fatal.
     * loading its own task segment is also how it learns its own name
     */
    gdt_load_here();
    if (!tss_init_cpu(me->index)) {
        ap_say('!');
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    idt_load_here();

    /*
     * star, lstar, sfmask and the gs bases are every one of them per
     * core. a thread that got scheduled onto a core without them would
     * execute `syscall` and jump to address zero, in ring 0, on the
     * user's stack, which is exactly as bad as it sounds
     */
    syscall_init();
    ap_say('I');

    lapic_enable_here();

    /*
     * out of its own local apic, which is the one thing this core cannot
     * be wrong about. if this matches what the firmware said, the code
     * really ran where it was sent
     */
    me->reported_id = lapic_id();
    me->online = true;
    ap_say('J');

    /*
     * an idle thread of its own, and then into the scheduler. from here
     * this core is a peer: it takes whatever is ready out of the same
     * ring the first one does
     */
    char name[8] = { 'i', 'd', 'l', 'e', (char)('0' + (me->index % 10)), 0 };
    if (!sched_join(me->index, name)) {
        ap_say('?');
        for (;;) {
            __asm__ volatile ("cli; hlt");
        }
    }
    me->scheduling = true;
    ap_say('K');

    /*
     * its own timer. the local apic is part of the processor, so every
     * core has one and each has to be started where it runs
     */
    lapic_timer_start(SMP_TICK_HZ, me->timer_ticks_per_second);

    /*
     * and let the world in. everything from here is ordinary: the timer
     * arrives, the scheduler picks something, and this core starts doing
     * the same work as any other
     */
    __asm__ volatile ("sti");
    for (;;) {
        __asm__ volatile ("hlt");
    }
}


/*
 * XXX: two things wrong with these two words. shootdown_wanted is
 * written and never read, the wait below comparing against its own local
 * count, so it is dead and reads as though it meant something. and the
 * counter has no generation: the wait is bounded, so a shootdown that
 * gives up can leave an ipi still in flight, and the next one resets the
 * counter before that straggler lands and counts it as one of its own
 * answers. tag each round so a late answer to the previous one cannot be
 * taken for an answer to this one.
 */
static volatile uint32_t shootdown_wanted;
static volatile uint32_t shootdown_done;
static struct spinlock shootdown_lock = SPINLOCK("tlb", LOCK_RANK_DEVICE);

void smp_tlb_ipi(void)
{
    /*
     * reloading cr3 throws away every cached translation that is not
     * marked global. a targeted invlpg would be cheaper and needs the
     * address carried across, which is worth doing when there is
     * evidence it matters rather than because it sounds better
     */
    uint64_t cr3;
    __asm__ volatile ("movq %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("movq %0, %%cr3" :: "r"(cr3) : "memory");

    __atomic_fetch_add(&shootdown_done, 1, __ATOMIC_SEQ_CST);
    lapic_eoi();
}

void smp_tlb_shootdown(void)
{
    if (online <= 1 || !lapic_available()) {
        return;         /* nobody else is remembering anything */
    }

    /*
     * a core that has not joined the scheduler yet is still being
     * brought up, and the only mappings it changes are for pages it has
     * just been given, which no other core has ever had a reason to
     * translate. more to the point, the core that woke it is sitting in
     * a polling loop waiting for it, and would not answer an interrupt
     * until it stopped. asking anyway means waiting out the whole bound,
     * three times over, for an answer that was never needed
     */
    uint32_t here = smp_this_cpu();
    if (here < cpu_count && !cpus[here].scheduling) {
        return;
    }

    uint64_t flags = spin_lock_irq(&shootdown_lock);

    uint32_t others = 0;
    __atomic_store_n(&shootdown_done, 0, __ATOMIC_SEQ_CST);

    for (size_t i = 0; i < cpu_count; i++) {
        if (!cpus[i].online || cpus[i].index == here || !cpus[i].scheduling) {
            continue;
        }
        if (lapic_send_ipi(cpus[i].apic_id, SMP_IPI_TLB)) {
            others++;
        }
    }
    __atomic_store_n(&shootdown_wanted, others, __ATOMIC_SEQ_CST);

    /*
     * wait for every one of them. carrying on early would mean using a
     * mapping the kernel has taken away while somebody else still has it,
     * bounded, because a core that never answers must not take the
     * machine with it
     */
    /*
     * well short of what the spinlock is willing to wait for. a core
     * that has not answered by now is one that cannot, and waiting past
     * the point where somebody else declares *the kernel* stuck helps nobody
     */
    for (uint64_t spin = 0; others > 0 && spin < 20000000ull; spin++) {
        if (__atomic_load_n(&shootdown_done, __ATOMIC_SEQ_CST) >= others) {
            break;
        }
        __asm__ volatile ("pause");
    }

    spin_unlock_irq(&shootdown_lock, flags);
}


/*
 * a copy of the kernel's, plus the first two megabytes mapped where they
 * really are. the kernel maps none of low memory, and the trampoline is
 * down there
 */
static uint64_t build_climb_tables(void)
{
    uint64_t pml4 = vmm_new_address_space();
    if (pml4 == 0) {
        return 0;
    }

    uint64_t *mine = pmm_phys_to_virt(pml4);
    const uint64_t *kernel = pmm_phys_to_virt(vmm_kernel_pml4());
    for (int i = 0; i < 512; i++) {
        mine[i] = kernel[i];
    }

    if (!vmm_map_range(pml4, 0, 0, 2 * 1024 * 1024, PTE_WRITE)) {
        pmm_free(pml4);
        return 0;
    }
    return pml4;
}

/*
 * only the identity map is the kernel's to free, every other entry is shared
 * with the kernel's own tables and freeing those would take the machine
 * with it
 */
static void free_climb_tables(uint64_t pml4)
{
    if (pml4 == 0) {
        return;
    }
    uint64_t *top = pmm_phys_to_virt(pml4);
    uint64_t pdpt = top[0] & 0x000ffffffffff000ull;
    if (pdpt != 0) {
        uint64_t *entries = pmm_phys_to_virt(pdpt);
        uint64_t pd = entries[0] & 0x000ffffffffff000ull;
        if (pd != 0) {
            pmm_free(pd);
        }
        pmm_free(pdpt);
    }
    pmm_free(pml4);
}


static void write_arg(uint64_t offset, uint64_t value)
{
    uint64_t *at = pmm_phys_to_virt(SMP_TRAMPOLINE_ADDR + offset);
    *at = value;
}

static bool wake(struct cpu *c, uint64_t climb_pml4, bool loud)
{
    uint64_t stack = pmm_alloc_pages(AP_STACK_PAGES);
    if (stack == 0) {
        return false;
    }
    c->stack_top = (uint64_t)pmm_phys_to_virt(stack)
                 + AP_STACK_PAGES * PAGE_SIZE;

    ap_loud = loud;
    write_arg(SMP_ARG_LOUD, loud ? 1 : 0);

    write_arg(SMP_ARG_CR3, climb_pml4);
    write_arg(SMP_ARG_STACK, c->stack_top);
    write_arg(SMP_ARG_ENTRY, (uint64_t)&smp_ap_entry);
    write_arg(SMP_ARG_CPU, (uint64_t)c);

    /*
     * whatever this core is running with, so the woken one matches. the
     * bit that matters is no-execute: the kernel's page tables set bit
     * 63 on everything that is not code, and on a core that has not
     * enabled the feature that bit is reserved, which faults on every
     * access rather than politely doing nothing
     */
    write_arg(SMP_ARG_EFER, rdmsr(0xc0000080) & 0xffffffffull);

    /*
     * the sequence is fixed by the manual and is not negotiable: assert
     * INIT, wait, then STARTUP twice. the second is not superstition,
     * some processors miss the first, and sending it to one that already
     * started is harmless because it is no longer listening
     */
    if (!lapic_send_init(c->apic_id)) {
        return false;
    }
    pit_poll_wait(10);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!lapic_send_startup(c->apic_id, SMP_TRAMPOLINE_VECTOR)) {
            return false;
        }

        /*
         * a core that is coming up takes microseconds. a core that is
         * never coming up takes forever, and must not be allowed to
         */
        for (int ms = 0; ms < 50; ms++) {
            pit_poll_wait(1);
            if (c->online) {
                return true;
            }
        }
    }
    return c->online;
}


bool smp_init(const struct acpi_info *info, uint64_t timer_ticks_per_second)
{
    cpu_count = 0;
    online = 0;

    if (info == NULL || !info->found || !lapic_available()) {
        return false;
    }

    uint32_t me = lapic_id();
    size_t listed = info->cpu_count;
    if (listed > SMP_MAX_CPUS) {
        listed = SMP_MAX_CPUS;
    }

    for (size_t i = 0; i < listed; i++) {
        struct cpu *c = &cpus[cpu_count];
        memset(c, 0, sizeof *c);
        c->index = (uint32_t)cpu_count;
        c->apic_id = info->lapic_ids[i];
        c->bootstrap = (c->apic_id == me);
        c->timer_ticks_per_second = timer_ticks_per_second;
        if (c->bootstrap) {
            c->online = true;
            c->reported_id = me;
            c->scheduling = true;
            online++;
        }
        cpu_count++;
    }

    if (cpu_count <= 1) {
        kprintf("smp        : one cpu, which is all the firmware described\n");
        return false;
    }

    uint64_t climb = build_climb_tables();
    if (climb == 0) {
        kprintf("smp        : no memory for the page tables to wake anyone on\n");
        return false;
    }

    /* the code they will start in, put where the startup vector points */
    memcpy(pmm_phys_to_virt(SMP_TRAMPOLINE_ADDR), smp_trampoline,
           smp_trampoline_size);

    for (size_t i = 0; i < cpu_count; i++) {
        struct cpu *c = &cpus[i];
        if (c->bootstrap) {
            continue;
        }
        if (wake(c, climb, false)) {
            online++;
        } else {
            /*
             * it did not come up, so ask it again with its own commentary
             * turned on. the last letter it manages is the diagnosis, and
             * it is the only one available, a core with no interrupt
             * table cannot report a fault, it can only reset the machine
             */
            kprintf("smp        : cpu %u (apic %u) did not answer. asking "
                    "again, aloud: ", c->index, c->apic_id);
            bool second = wake(c, climb, true);
            kprintf(second ? ", awake on the second ask\n"
                           : ", and that is as far as it gets\n");
            if (second) {
                online++;
            }
        }
    }

    free_climb_tables(climb);

    kprintf("smp        : %zu of %zu cpus awake\n", online, cpu_count);
    for (size_t i = 0; i < cpu_count; i++) {
        const struct cpu *c = &cpus[i];
        if (!c->online || c->bootstrap) {
            continue;
        }
        if (c->reported_id != c->apic_id) {
            kprintf("smp        : cpu %u says it is apic %u, not %u. "
                    "something started the wrong core\n",
                    c->index, c->reported_id, c->apic_id);
        }
    }
    return online > 1;
}

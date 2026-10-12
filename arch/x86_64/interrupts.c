// SPDX-License-Identifier: GPL-2.0-only
/*
 * arch/x86_64/interrupts.c
 * Jose A. Perez de Azpillaga <azpijr@gmail.com>, 2026
 * the interrupt stubs' entry into c, and the frame they leave.
 */

#include <asm/interrupts.h>
#include <asm/pic.h>
#include <asm/acpi.h>
#include <asm/lapic.h>
#include <asm/smp.h>
#include <asm/ioapic.h>
#include <asm/idt.h>
#include <velvet/pit.h>
#include <velvet/kprintf.h>
#include <velvet/panic.h>
#include <velvet/backtrace.h>
#include <velvet/pmm.h>
#include <velvet/addrspace.h>
#include <velvet/console.h>
#include <velvet/sched.h>
#include <stddef.h>

/*
 * if isr.asm and the frame struct ever drift apart, fail the build
 * instead of debugging garbage register dumps at 2am
 */
_Static_assert(offsetof(struct interrupt_frame, vector) == 15 * 8,
               "interrupt_frame drifted from isr.asm push order");
_Static_assert(sizeof(struct interrupt_frame) == 22 * 8,
               "interrupt_frame drifted from isr.asm push order");

static const char *exception_names[32] = {
    "#DE divide error",
    "#DB debug",
    "NMI non-maskable interrupt",
    "#BP breakpoint",
    "#OF overflow",
    "#BR bound range exceeded",
    "#UD invalid opcode",
    "#NM device not available",
    "#DF double fault",
    "coprocessor segment overrun (how old is this cpu?)",
    "#TS invalid tss",
    "#NP segment not present",
    "#SS stack segment fault",
    "#GP general protection fault",
    "#PF page fault",
    "reserved (15)",
    "#MF x87 floating point",
    "#AC alignment check",
    "#MC machine check",
    "#XM simd floating point",
    "#VE virtualization",
    "#CP control protection",
    "reserved (22)", "reserved (23)", "reserved (24)", "reserved (25)",
    "reserved (26)", "reserved (27)",
    "#HV hypervisor injection",
    "#VC vmm communication",
    "#SX security",
    "reserved (31)",
};

static uint64_t read_cr2(void)
{
    uint64_t v;
    asm volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

static void dump_frame(struct interrupt_frame *f)
{
    kprintf("rax=%016lx rbx=%016lx rcx=%016lx\n", f->rax, f->rbx, f->rcx);
    kprintf("rdx=%016lx rsi=%016lx rdi=%016lx\n", f->rdx, f->rsi, f->rdi);
    kprintf("rbp=%016lx r8 =%016lx r9 =%016lx\n", f->rbp, f->r8, f->r9);
    kprintf("r10=%016lx r11=%016lx r12=%016lx\n", f->r10, f->r11, f->r12);
    kprintf("r13=%016lx r14=%016lx r15=%016lx\n", f->r13, f->r14, f->r15);
    kprintf("rip=%016lx rsp=%016lx rflags=%08lx\n", f->rip, f->rsp, f->rflags);
    kprintf("cs=%02lx ss=%02lx err=%lx\n", f->cs, f->ss, f->error_code);
}

static void (*irq_handlers[16])(void);

/*
 * two separate questions, and conflating them is what broke the
 * keyboard: whether the *timer* comes from the lapic, and whether
 * *external* interrupts come from the io apic. the first
 * happens at boot and the second only if asked, so for a while the
 * answer is yes to one and no to the other, and an interrupt has to
 * be acknowledged at whichever chip actually delivered it
 */
static bool timer_on_lapic;
static bool external_on_ioapic;
static struct acpi_info acpi;

const struct acpi_info *interrupts_acpi(void)
{
    return &acpi;
}

static uint64_t timer_rate;

uint64_t interrupts_timer_rate(void)
{
    return timer_rate;
}

bool interrupts_on_apic(void)
{
    return timer_on_lapic;
}

void irq_install(uint8_t line, void (*handler)(void))
{
    if (line >= 16) {
        return;
    }
    irq_handlers[line] = handler;
    pic_unmask(line);

    /*
     * lines 8..15 are on the second pic, and the second pic reaches the
     * cpu through line 2 of the first. a device that never interrupts on
     * a machine that has one is almost always this, it lived in
     * mouse.c, where it was correct and invisible to the
     * next driver that needed it
     */
    if (line >= 8) {
        pic_unmask(2);
    }
}

static void interrupt_handle(struct interrupt_frame *f);

/*
 * every interrupt and exception comes through here, counted on the thread
 * it interrupted while it is handled. the count is the thread's own
 * variable, so it is still the right thread's when the handler switched
 * to another and back before returning
 */
void interrupt_dispatch(struct interrupt_frame *f)
{
    struct thread *p_thread = sched_current();

    if (p_thread != NULL) {
        p_thread->u_in_interrupt++;
    }
    interrupt_handle(f);
    if (p_thread != NULL) {
        p_thread->u_in_interrupt--;
    }
}

static void interrupt_handle(struct interrupt_frame *f)
{
    /*
     * the apic timer arrives on a vector of its own, above the range
     * the 8259 was remapped into
     */
    if (f->vector == LAPIC_TIMER_VECTOR) {
        lapic_eoi();
        /*
         * the same tick, arriving by a different road. everything above
         * counts in these, so the change must be invisible from there
         */
        pit_tick();
        return;
    }

    /*
     * another core changed a page table and this one still remembers the
     * old translation. nothing in the hardware would have told it
     */
    if (f->vector == SMP_IPI_TLB) {
        smp_tlb_ipi();
        return;
    }

    /*
     * an apic raises this when an interrupt is withdrawn before it can
     * be delivered. it gets no eoi, acknowledging one that never
     * happened puts the controller out of step
     */
    if (f->vector == LAPIC_SPURIOUS_VECTOR) {
        return;
    }

    if (f->vector >= PIC_IRQ_BASE && f->vector < PIC_IRQ_BASE + 16) {
        uint8_t irq = f->vector - PIC_IRQ_BASE;

        /*
         * the 8259's ghosts are only the 8259's problem, and it is
         * still the one delivering these unless the kernel moved them
         */
        if (!external_on_ioapic && pic_is_spurious(irq)) {
            return;
        }
        /*
         * eoi goes BEFORE the handler, which looks wrong until you
         * remember the scheduler exists: the timer handler can switch
         * threads and never come back on this stack. a freshly created
         * thread has no half-finished irq frame to return through, so
         * it would never send the eoi and the controller would go quiet
         * forever. interrupts are off in here (interrupt gate), so
         * nothing can nest before the kernel iretqs
         */
        /*
         * acknowledge whichever chip actually raised it. getting this
         * backwards means the 8259 never hears that its interrupt was
         * handled, and quietly stops delivering any more
         */
        if (external_on_ioapic) {
            lapic_eoi();
        } else {
            pic_send_eoi(irq);
        }
        if (irq_handlers[irq]) {
            irq_handlers[irq]();
        } else {
            kprintf("irq %u fired with nobody listening\n", irq);
        }
        return;
    }

    if (f->vector >= 32) {
        /* not an exception, not a pic line. shouldnt happen, dont die over it */
        kprintf("stray interrupt %lu, ignoring\n", f->vector);
        return;
    }

    struct thread *me = sched_current();

    /*
     * the page fault handler stops being purely an error path.
     *
     * a fault here is usually not a mistake at all. two of them are the
     * mechanism rather than the failure: a write to a page two spaces
     * were sharing, which means give this one a private copy; and a
     * touch of a page that was never there, which means make it. either
     * way the instruction runs again and the program never finds out.
     *
     *
     * checked before anything is printed, because the overwhelming
     * majority of faults from here on are this and nobody wants a log
     * line per page
     */
    if (f->vector == 14 && me != NULL && me->space != NULL) {
        uint64_t cr2 = read_cr2();
        bool present = (f->error_code & 1) != 0;
        bool write = (f->error_code & 2) != 0;
        if (addrspace_fault(me->space, cr2, write, present)) {
            return;
        }
    }

    /*
     * FIXME: a fault from ring 3 ends the machine rather than the
     * program. nothing from here on looks at f->cs, so a user process
     * that executes an illegal opcode, divides by zero, or touches an
     * address outside its own space falls through to the panic at the
     * bottom and takes the kernel down with it, from one line any
     * logged-in guest can run. SIGSEGV, SIGILL and SIGFPE already exist,
     * and process_died_by_signal and thread_exit do the right thing for
     * the SIGPIPE in sys_write, so the machinery is here and the fault
     * path simply never reaches for it. when (f->cs & 3) == 3, raise the
     * signal this vector maps to against the current process and take
     * its default action the way deliver_signals does, and leave the
     * panic below for the faults that are genuinely the kernel's.
     */

    /* cpu exception. print everything the kernel knows, then panic */
    console_set_colors(0xe64553, 0x101018);

    /*
     * a double fault means the cpu couldnt even deliver the first
     * exception, almost always because rsp was already somewhere
     * unusable. the kernel is only alive to say so because the idt sends this
     * vector to its own IST stack. cr2 still holds whatever address
     * the original fault was about, which is the useful part
     */
    if (f->vector == 8) {
        uint64_t cr2 = read_cr2();
        kprintf("\n\ndouble fault: the cpu could not deliver an exception\n");
        if (me != NULL) {
            kprintf("in thread %d (%s)\n", me->id, me->name);
            if (me->stack_phys != 0) {
                uint64_t guard = (uint64_t)pmm_phys_to_virt(me->stack_phys);
                if (cr2 >= guard && cr2 < guard + PAGE_SIZE) {
                    kprintf("the first fault was at %p, this thread's stack "
                            "guard page.\nit ran out of stack, the guard did "
                            "its job, and the IST caught the fallout\n",
                            (void *)cr2);
                }
            }
        }
        kprintf("first fault was about %p\n", (void *)cr2);
        dump_frame(f);
        kbacktrace(f->rbp, f->rip);
        panic("double fault (running on the IST stack)");
    }
    kprintf("\n\ncpu exception %lu: %s\n", f->vector, exception_names[f->vector]);
    if (me != NULL) {
        kprintf("in thread %d (%s)\n", me->id, me->name);
    }

    if (f->vector == 14) {
        uint64_t cr2 = read_cr2();
        uint64_t e = f->error_code;
        kprintf("page fault at %p: %s during %s%s in %s mode\n",
                (void *)cr2,
                (e & 1) ? "protection violation" : "page not present",
                (e & 16) ? "instruction fetch" : ((e & 2) ? "write" : "read"),
                (e & 8) ? " (reserved bit set?!)" : "",
                (e & 4) ? "user" : "kernel");

        /*
         * a fault just below a thread's stack is almost always the
         * guard page doing its job rather than a wild pointer
         */
        if (me != NULL && me->stack_phys != 0) {
            uint64_t guard = (uint64_t)pmm_phys_to_virt(me->stack_phys);
            if (cr2 >= guard && cr2 < guard + PAGE_SIZE) {
                kprintf("that is this thread's stack guard page, "
                        "it ran out of stack\n");
            }
        }
    }

    dump_frame(f);

    /*
     * the interesting stack is the one that faulted, not the kernel's. rip
     * goes in separately: the faulting instruction never made it onto
     * the frame chain
     */
    kbacktrace(f->rbp, f->rip);

    panic("%s at rip=%016lx", exception_names[f->vector], f->rip);
}


/*
 * put everything back the way it was. this exists because the failure
 * mode here is a machine with no keyboard, and a machine with no
 * keyboard cannot be told to try something else, so every doubt has
 * to resolve itself before the boot finishes, not after
 */
static void back_to_the_8259(const char *why)
{
    kprintf("interrupts : %s. putting the 8259 and the pit back\n", why);
    timer_on_lapic = false;
    external_on_ioapic = false;

    pic_init();
    pit_init();
    pic_unmask(1);      /* keyboard */
    pic_unmask(4);      /* serial */
}

bool interrupts_use_apic(void)
{
    acpi = acpi_init();
    if (!acpi.found || acpi.lapic_address == 0 || acpi.ioapic_count == 0) {
        return false;       /* the 8259 keeps the job */
    }

    if (!lapic_init(acpi.lapic_address)) {
        return false;
    }
    if (!ioapic_init(acpi.ioapics[0].address, acpi.ioapics[0].gsi_base)) {
        kprintf("interrupts : the io apic did not answer\n");
        return false;
    }

    /*
     * measure the lapic timer *first*, while everything old still
     * works. it is driven by the bus clock, whose speed nobody
     * documents, so it has to be counted against something that
     * already knows what a second is.
     *
     * the wait polls the pit rather than counting its interrupts,
     * which matters twice over: interrupts are still off this early in
     * boot, and the kernel is about to mask the very chip that would deliver
     * them. a wait that needed either would spin here forever
     */
    uint64_t hz = lapic_calibrate(pit_poll_wait, 50);
    timer_rate = hz;
    if (hz < 1000 || hz > 100000000000ull) {
        /*
         * an answer that absurd means the measurement failed, and a
         * timer started from it would be worse than the pit
         */
        kprintf("interrupts : lapic timer measured %lu Hz, which cannot be "
                "right. staying on the pit\n", hz);
        return false;
    }

    /*
     * only the timer line goes. everything else stays on the 8259,
     * see the note above interrupts_use_ioapic() for why that is a
     * decision rather than an omission. two timers would both fire,
     * so this one has to be silenced either way
     */
    pic_mask(0);

    lapic_timer_start(PIT_HZ, hz);
    pit_stop();
    /*
     * and now prove it, before anything depends on it.
     *
     * everything above this point is arithmetic and register writes
     * that either worked or did not, and there is no way to tell from
     * here. so let interrupts in briefly, wait by a means that needs no
     * interrupt at all, and see whether the new timer actually
     * delivered anything. if it did not the kernel can still put the old one
     * back; ten lines later the kernel could not, and the symptom would be a
     * machine that boots to a prompt and then never sleeps again
     */
    uint64_t before = pit_ticks();
    asm volatile ("sti");
    pit_poll_wait(30);
    asm volatile ("cli");

    if (pit_ticks() == before) {
        back_to_the_8259("the lapic timer was started and delivered nothing");
        return false;
    }
    timer_on_lapic = true;
    kprintf("interrupts : lapic %u, timer at %lu Hz measured against the pit\n",
            lapic_id(), hz);
    kprintf("             timer acknowledged at the lapic, everything else "
            "still at the 8259\n");
    return true;
}

/*
 * this half is not done at boot, and the reason is worth writing down
 * rather than leaving as an apparent oversight.
 *
 * the timer can be proved: start it, wait by other means, see whether
 * it delivered. an io apic route cannot. the registers can be read back
 *, and are, but a redirection entry that reads back perfectly and
 * still delivers nothing is an ordinary failure, and the way you find
 * out is that the keyboard stops. a machine with no keyboard cannot be
 * told to try something else.
 *
 * so it lives behind a command instead. from a shell that already works
 * you can ask for it, and if the keyboard goes quiet a reboot puts you
 * back exactly where you were. that is a worse default and a much
 * better way to find out.
 */
bool interrupts_use_ioapic(void)
{
    if (!timer_on_lapic) {
        kprintf("the lapic is not running; there is nothing to route to\n");
        return false;
    }
    if (!ioapic_available()) {
        kprintf("no io apic answered at boot\n");
        return false;
    }

    static const uint8_t wired[] = { 1, 4 };    /* keyboard, serial */
    uint32_t id = lapic_id();

    for (size_t w = 0; w < sizeof wired; w++) {
        uint8_t irq = wired[w];
        uint32_t gsi = acpi_gsi_for_irq(&acpi, irq);

        uint16_t flags = 0;
        for (size_t i = 0; i < acpi.override_count; i++) {
            if (acpi.overrides[i].isa_irq == irq) {
                flags = acpi.overrides[i].flags;
            }
        }

        if (!ioapic_route(gsi, (uint8_t)(PIC_IRQ_BASE + irq), id, flags)) {
            kprintf("routing irq %u failed; nothing has been changed\n", irq);
            return false;
        }
        kprintf("  irq %u -> line %u -> vector %u\n",
                irq, gsi, PIC_IRQ_BASE + irq);
    }

    /*
     * XXX: this masks every line, including the fourteen that were not
     * just routed. a device whose irq is not in that short list, and
     * whose driver is listening on the 8259, goes quiet here and never
     * says so: on this machine the network card is on irq 11, so asking
     * for the io apic drops it to its polling fallback, which works and
     * is slower. the note above explains why the old chip is silenced
     * rather than merely masked, and says nothing about what else was
     * still on it. route the rest, leave them alone, or say this in the
     * note.
     */
    /*
     * only now silence the old chip. a masked 8259 still asserts its
     * line on some hardware, so it goes entirely rather than quietly
     */
    for (uint8_t i = 0; i < 16; i++) {
        pic_mask(i);
    }
    /*
     * only now does an external interrupt get acknowledged at the
     * lapic, because only now is the lapic the one delivering it
     */
    external_on_ioapic = true;
    kprintf("the 8259 is masked. if the keyboard has gone quiet, reboot\n");
    return true;
}

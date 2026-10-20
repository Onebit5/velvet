the architecture boundary
=========================

``arch/`` is the line between the code that would run on any
machine and the code that will only ever run on this one. it is the most
important structural rule in the kernel, and the build enforces it: see
`code style <../coding-style.rst>`_ for the plain statement.

the sentence it exists to make true is *everything above this directory is
machine-independent.* that sentence was not true before 0.2.20. the kernel
named registers all over the tree. ``mov %%cr3`` in the address space
code, ``mov %%rbp`` in the backtrace, ``hlt`` in the scheduler, ``pause`` in
three places, ``sti`` in two, and nineteen files including an x86 header to
get ``irq_save``.

drawn from the outside in
-------------------------

the headers here are named for **what the rest of the kernel wants**, not
for what x86 happens to provide. that is the whole difference between a
boundary and a folder. drawn the other way round, ``arch/`` would have
ended up with a ``write_cr3()``. an x86 instruction with a
portable-looking name, which is worse than the inline assembly it replaced,
because at least the inline assembly was honest about what it was.

the five headers are:

``<asm/irq.h>``
   may interrupts happen right now. ``irq_install`` routes a device's
   interrupt and lets it through in one call, because every caller used to
   register and forget to unmask. ``irq_save`` / ``irq_restore`` nest.
   a blind disable/enable pair cannot, because the inner one turns them
   back on while the outer caller still needed them off.

``<asm/cpu.h>``
   what this core can be told to do. ``cpu_relax`` (this is a spin, not
   work), ``cpu_idle`` (stop until something happens. interrupts must be
   on), ``cpu_stop`` (the deliberate version), and the two "where am i"
   calls a backtrace and ``vmm`` need: ``cpu_frame_pointer`` and
   ``cpu_stack_pointer``.

``<asm/mmu.h>``
   the three moments where what was written down has to be *believed by a
   processor*: ``mmu_load_table`` (run on these tables),
   ``mmu_flush_page`` (one translation is stale), and
   ``mmu_enforce_write_protect`` (make read-only mean read-only for the
   kernel too). building tables, walking them and splitting huge pages are
   arithmetic over memory and stay above this line, even though all of it
   is four-level and x86-shaped today.

``<asm/machine.h>``
   the *box*, rather than the processor. two machines with the same
   instruction set stop and restart in entirely different ways, so this is
   separate on purpose. ``machine_bring_up_early`` (what has to exist
   before there is memory to allocate from), ``machine_bring_up_late``
   (what needs the allocators first), ``machine_start_clock`` (the timer,
   and the other cores), ``machine_reset``, ``machine_poweroff`` and
   ``machine_key_pressed`` (asked with interrupts off, for the panic
   screen). these three bring-up calls are what ``kmain`` used to *be*.

``<asm/context.h>``
   what a parked thread's state is, so the scheduler does not name a
   register file.

every architecture provides all five, in ``arch/<arch>/include/asm/``,
and the build puts exactly one of those directories on the include path:
``-Iarch/$(ARCH)/include``. which machine ``<asm/cpu.h>`` means is decided
there and nowhere else, so no header carries a list of architectures. a
second architecture that is missing one of the five fails to compile, with
the name of the header it lacks, rather than the machine booting and being
subtly wrong.

``ARCH_INLINE``, in ``<velvet/compiler.h>``, is not part of the contract
at all. an arch primitive has to *be* its instruction where it is written: the kernel builds
at ``-O0``, where a plain ``static inline`` is a call like any other, and
for the two that read a register a call is not merely slower; it would
read its own frame and its own stack, and the backtrace would politely
report itself.

x86_64
------

``arch/x86_64/`` is the only real implementation, and everything the rest
of the kernel needs from this machine lives here. its headers are in
``arch/x86_64/include/asm/``, which is what ``<asm/...>`` means when the
kernel is built for it:

- ``gdt.c`` / ``asm/gdt.h``. the descriptor tables, including the task state
  segment slot that has been reserved since the first exception handler.
- ``idt.c`` / ``asm/idt.h``, ``interrupts.c``, ``isr.asm``. the exception and
  interrupt vectors, and the stubs that save a frame before the C handler.
- ``asm/irq.h``, ``pic.c`` (the 8259), ``ioapic.c``, ``lapic.c``, ``acpi.c``.
  interrupt routing, from the legacy pair up to the modern hardware.
- ``smp.c``, ``trampoline.asm``. waking the other cores in real mode and
  bringing them into long mode.
- ``tss.c``. the task state segment, whose interrupt stack table is what
  lets a stack overflow report instead of triple-faulting, and whose
  ``rsp0`` is the stack a ring 3 trap lands on.
- ``syscall.asm`` / ``syscall.c``, ``usermode.asm``. the way in from
  ring 3.
- ``switch.asm``. the context switch, six callee-saved registers and a
  return address.
- ``cpuinfo.c``, ``machine.c``, ``asm/msr.h``, ``asm/io.h``, ``uart.c``. the
  processor, the box, and the port i/o that makes a file visibly x86.
- ``none/``. the architecture that does nothing; below.

none: an instrument, not a port
-------------------------------

``arch/none/`` will never run. it is an **instrument**, and it measures one
thing: whether the boundary is real or is decoration.

``tools/checkarch.py`` greps; it can see an ``#include`` of an
``<asm/...>`` header that ``arch/none`` does not provide, which is what an
x86 header is, and a line of inline assembly, and it cannot see a portable file that
quietly *depends* on something only x86 provides. so ``make
portable-check`` compiles the whole portable kernel. ``mm/``, ``sched/``,
``fs/``, ``lib/``, most of ``drivers/``, against
``arch/none/include/asm/``, which supplies the five contracts and nothing
else. every function is empty, every
constant a plausible-looking lie.

if it compiles, nothing above the line needs a machine. if it does not, the
compiler names the leak and the line number.

the objects are then linked and the *undefined symbols* collected, and what
comes back is the porting checklist. every name a port would have to
define. derived mechanically rather than written by somebody trying to
remember. ``make portable-check LIST=1`` prints it.

why the values here are wrong on purpose: ``cpu_id()`` returns 0,
``cpu_stack_pointer()`` returns 0, ``CPU_MAX`` is 1. none of these are
attempts to be reasonable. they are the smallest thing that satisfies the
type, because the moment one looks like a *plausible* answer somebody will
start relying on it, and this directory has to stay useless in order to stay
honest.

what is deliberately still x86 and should not be
------------------------------------------------

the drivers that talk to ports. the 8259, the pit, ps/2, the cmos clock,
the 8250 uart, pci configuration. are still in ``drivers/``. they are as
x86 as anything in this directory. they are not moved because **a boundary
drawn around drivers before there is a second machine to draw it against is
a guess.**

one of them stopped being a guess. an aarch64 port was written and then
removed, in 0.2.20, and while it
existed it answered the question
for the serial driver: ``drivers/serial.c`` was a terminal with a chip stuck
to it. an 8250 is not an x86 chip. *reaching* it through a port space is,
so the escape-sequence machine stayed in ``drivers/`` and the ``outb``
instructions went to ``arch/x86_64/uart.c``. the rest are still here because the port was
removed before it could say anything about them.

what is done in the meantime is making the leak *legible and counted*: port
i/o lives in ``<asm/io.h>``, which only x86 has, so a file that does it
visibly includes an x86 header; ``tools/checkarch.py`` prints the list on every build; and
``tools/portable.py`` catches the kind that has no include to grep for.

the contract each header states
-------------------------------

what follows is interface documentation: what each header promises,
rather than how the code behind it works, which is what the comments
in the implementations are for. it lived in the headers until it was
clear that a header is read by people compiling against it, not only
by people porting it.

``<asm/cpu.h>``::

    void cpu_relax(void)
    void cpu_idle(void)
    void cpu_stop(void)  [noreturn]
    uint64_t cpu_frame_pointer(void)
    uint64_t cpu_stack_pointer(void)
    void cpu_show(void)

``cpu_show`` writes, with kprintf, what the processors are and which of them
are awake: what ``cpus`` prints, and ``/proc/cpuinfo``, which ``fs/proc.c``
registers for every architecture. one with no table of processors says so.

``<asm/irq.h>``::

    void irq_install(uint8_t line, void (*handler)(void))
    uint64_t irq_save(void)
    void irq_restore(uint64_t flags)
    void irq_enable(void)
    void irq_disable(void)

``<asm/machine.h>``::

    void machine_bring_up_early(void)
    void machine_bring_up_late(void)
    void machine_start_clock(void)
    void machine_reset(void)  [noreturn]
    bool machine_poweroff(void)
    bool machine_key_pressed(void)

``<asm/mmu.h>``::

    void mmu_load_table(uint64_t phys_root)
    void mmu_flush_page(uint64_t virt)
    void mmu_enforce_write_protect(void)

``<asm/context.h>``::

    uint64_t context_make_stack(uint64_t stack_top, void (*entry)(void))
    void switch_context(uint64_t *save_sp, uint64_t *load_sp)
    void context_set_kernel_stack(uint64_t top)
    void context_enter_user(entry, stack_top, argc, argv)  [noreturn]


none of these are attempts to be reasonable. they are the smallest thing that satisfies the type, because the moment one of them looks like a *plausible* answer somebody will start relying on it, and this directory has to stay useless in order to stay honest. a stub that returned a believable stack pointer would be a stub that hides the file which needed a real one.

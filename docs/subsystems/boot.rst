philemon, the bootloader
========================

``boot/`` is the bootloader, and it is this project's own. it was a
borrowed one until 0.1.12; writing one and then booting with somebody
else's is not much of a bootloader, so that one is gone, and so are the
iso, the uefi path and the boot protocol that came with it.

philemon is named for the one who grants the power and then steps back. he
does not fight anything and he is not there for the rest of it. that is the
whole job description of a bootloader, and it is more than most of them get.

he does three things:

#. asks the bios everything it will ever be able to answer, because the
   moment long mode starts it stops answering;
#. turns the kernel from a file into something running at the address it
   was linked for;
#. leaves behind one struct describing the machine.

one program, one sector
-----------------------

the only reason there is a line across the middle of ``philemon.asm`` is
that the bios reads exactly one sector. 512 bytes ending in ``0x55
0xaa``. drops it at ``0x7c00`` and jumps to it. that is the entire
contract and it is not negotiable. so the first 512 bytes do nothing but
pull in the rest of the same file, to the address immediately after
themselves, and carry on. after that the line is invisible: it is one image
at one address, and nothing below cares where the sector ended.

everything the bios can be asked, he asks while it can still answer:

a20
   the twenty-first address line is still disabled at power on so a machine
   from 1981 could wrap around at one megabyte. it has been forty years.

unreal mode
   the bios cannot write above one megabyte and the kernel does not fit
   below it. so: into protected mode for exactly long enough to load one
   segment register with a descriptor whose limit is the whole address
   space, and back out again. returning to real mode does not reload the
   hidden half of a segment register, so the wide limit survives and 32-bit
   offsets keep working. it goes in ``fs``, not ``es``, and that detail is
   the difference between this working and not.

page tables
   long mode will not start without them. two-megabyte pages, three
   windows: everything identity mapped so the loader keeps working, the
   same memory again in the higher half where the kernel expects a direct
   map, and the kernel's own window at the top.

the 64-bit half is C, and that is the point of the split. once long mode
runs there is no reason to stay in assembly, and the work left. parsing an
elf, honouring the distances a linker chose between segments, zeroing bss,
carving everything already spent out of the memory map. is exacting work
where being wrong by eight bytes is a black screen. all of it compiles for
the host and is tested (``tests/test_philemon.c``), which the assembly
cannot be.

the handoff
-----------

the handoff is one struct, ``struct ph_handoff`` in ``philemon.h``, and the
kernel is entered the way any function is called: a pointer to it in
``rdi``. there is no protocol to speak of and nothing to scan for.
``init/boot.c`` keeps a pointer rather than copying it, because it
sits in memory philemon marked reclaimable, which the kernel does not
reclaim until it is finished with all of it.

everything the kernel could only have learned before long mode is in there:

- the direct map offset (``hhdm``). add it to a physical address;
- where the kernel really is and where it thinks it is;
- the memory map, already sorted, with everything spent carved out;
- the ramdisk, through the direct map;
- ``rsdp``, or 0 if the firmware has no acpi tables;
- the framebuffer, or a width of 0 if no video mode was got.

there is a smaller, rawer struct before that, ``struct ph_early``, which is
the bios's own account of the machine. the e820 map and the framebuffer.
left by the 16-bit half for the 64-bit half to make sense of.

the table on the disk
---------------------

``tools/mkboot.py`` writes a table into the image saying where each piece
is. it sits at logical block 32, because the first sectors are spoken for.
the table names the handoff, the kernel, the ramdisk, and the project's own
**source tree**, which philemon does not load and never will. the kernel
and the ramdisk have to be in memory before anything runs; the source has
to be *reachable*, which is a different and much cheaper requirement, so it
stays on the medium and is read a sector at a time. see `the source on the
medium <fs.rst>`_ for the other half of that.

the table only ever grows at the end, which is why three fields of zeroes
mean "no source tree" and an image from 0.3.25 still reads. an older loader
reads the fields it knows and stops; a newer kernel reading an older table
finds zeroes, which is the honest answer.

the parts worth testing
-----------------------

``ph_load_elf()`` copies an elf's loadable segments to where they belong,
moving all of them by the same amount so the distances the linker chose
survive. ``ph_build_memmap()`` turns the bios's account into the kernel's.
``ph_find_rsdp()`` finds the acpi tables where the firmware leaves them.
these three are the hard ones, and all three take their input as arguments,
so the host suite can fabricate a machine and poke at them.

the assembly says what it is doing at every step, over the serial port, from
the first instruction of the boot sector onwards. a bootloader that fails
silently is one nobody can fix.

the boot interface
--------------------

the handoff. philemon leaves one struct at a known place describing the
machine, and the kernel is entered with a pointer to it in rdi, the way any
function is called. there is no protocol to speak of and nothing to scan for.

everything the kernel needs to know about the hardware it woke up on is in
there, because the moment long mode starts, the firmware stops answering
questions.

``void boot_take_handoff(const struct ph_handoff *h);``
    what philemon left behind.

``uint64_t boot_hhdm(void);``
    the direct map offset, which everything needs and nothing should have to go through the struct for

the boot protocol
=================

how a loader starts this kernel: what it has done by the time the kernel
runs, where it has put things, and the one struct it leaves behind.
``include/velvet/boot.h`` declares all of it, and this page is what a loader
has to do to satisfy it.

the protocol is the kernel's. a loader that does what this page says boots
the kernel, whoever wrote it. philemon is the loader this tree's image is
written with, and today he is the only one that speaks it; another loader
that does not, a multiboot one or one that boots linux, does not start this
kernel at all. there is no second entry point to fall back on.

it is **revision 1**, and its magic is ``PHLMN0``, which is philemon's
name for it from before the protocol was the kernel's. every image
already written carries that magic, so it stays.

the state the kernel is entered in
----------------------------------

- **long mode**, paging on, interrupts off. the segments are flat and of
  the loader's making; the kernel loads its own gdt, idt and tss before it
  relies on any of them.
- **the page tables** map, with any page size:

  - physical memory from 0 at ``BOOT_HHDM``, ``0xffff800000000000``, at
    least as far as everything the handoff points at. this is the direct
    map, and the kernel reads every pointer in the handoff through it;
  - the kernel's loadable segments at the virtual addresses they were linked
    for, in the top two gigabytes, ``0xffffffff80000000`` and up;
  - and whatever the loader needs to keep running long enough to jump,
    which philemon does with an identity map of the first four gigabytes.
    the kernel uses none of it, and replaces the whole of it with tables of
    its own (``mm/vmm.c``) before it frees the loader's memory.

- **rsp** is a stack in mapped memory the map does not offer as free,
  with a zero return address on it, and **rbp** is 0, so a backtrace stops
  there.
- **rdi** is the handoff, through the direct map. the kernel is entered at
  its elf entry point the way any function is called, and it does not
  return.

what the loader has done
------------------------

#. **the kernel**, an elf64 executable, loaded by its program headers:
   every ``PT_LOAD`` segment copied into physical memory, all of them moved
   by the same amount so the distances the linker chose between them
   survive, and the part of each that the file does not hold zeroed. where
   it lands is the loader's choice, as long as the kernel window maps it;
   the kernel learns it from ``kernel_phys`` and ``kernel_virt`` and assumes
   nothing else. philemon puts it at 32 MiB.
#. **the ramdisk**, a ustar archive, loaded whole, anywhere. it is never
   moved and never freed: a program on it is run straight out of it.
#. **the memory map**: everything the firmware described, typed with
   ``enum boot_memory``, sorted by base. what the loader used itself is
   carved out as ``BOOT_MEM_LOADER``, which the kernel takes back once it
   has stopped reading the handoff, and the kernel and the ramdisk as
   ``BOOT_MEM_KERNEL``, which it never does.
#. **the acpi tables**: the rsdp, found where the firmware leaves it, or 0.
#. **the screen**: a linear framebuffer mode if the firmware has one, and
   its geometry. a width of 0 says there was none, and the kernel carries on
   with the serial line alone.
#. **the handoff** itself, filled in, in memory the map types as the
   loader's.

the handoff
-----------

``struct boot_handoff``, every field 64 bits wide but the framebuffer's:

``magic``, ``revision``
   ``BOOT_MAGIC`` and ``BOOT_REVISION``. the kernel checks the magic before
   anything else, and stops dead without it: there is no console yet to say
   why on.
``hhdm``
   where the direct map starts, ``BOOT_HHDM``.
``kernel_phys``, ``kernel_virt``
   where the kernel's lowest segment is in physical memory, and the address
   it was linked for.
``memmap``, ``memmap_count``
   the map, an array of ``struct boot_memmap_entry``, through the direct
   map.
``ramdisk``, ``ramdisk_size``
   the archive, through the direct map, or 0 and 0 for none.
``rsdp``
   the rsdp's *physical* address, or 0. it is the one pointer in the
   struct that does not go through the direct map, which is a wart: the
   kernel takes either and tells them apart by which side of ``hhdm`` they
   fall, and a loader written now should give the physical one.
``fb``
   ``struct boot_framebuffer``: the address through the direct map, the
   pitch in bytes, the size in pixels, the bits per pixel, and where each
   colour sits in a pixel and how wide it is.

the table on the medium
-----------------------

a medium the kernel was booted from carries ``struct boot_table`` at
logical block 32, ``BOOT_TABLE_LBA``, beginning with the same magic. the
kernel reads only the magic, and only to know which drive it was booted
from, which is a question nothing in the handoff answers: a drive number
from the bios means nothing once the kernel is talking to the disk
controller itself. a medium without the table still boots, and the kernel
says it was started by something else.

the rest of the table is the loader's: where its own second half is, the
kernel and the ramdisk, and a source archive for whatever runs later, three
fields of zeroes when there is none. the table only ever grows at the end,
so an older reader stops at the fields it knows.

the loader this tree builds with
--------------------------------

``boot/`` is philemon, and ``tools/mkboot.py`` writes his image: his first
sector, the rest of him, the table at block 32, then the kernel and the
ramdisk. he declares the protocol in ``include/velvet/philemon.h`` under
names of his own, and ``tests/test_boot.c`` holds the two declarations
against each other field by field, because they describe the same bytes and
neither can include the other.

the boot interface
------------------

``void boot_take_handoff(const struct boot_handoff *h);``
    keep a pointer to what the loader left, after checking the magic. kept
    rather than copied, because it sits in memory the kernel does not take
    back until it has finished with all of it.

``const struct boot_handoff *boot_handoff(void);``
    the handoff, for anybody who needs a field of it later.

``uint64_t boot_hhdm(void);``
    the direct map offset, which everything needs and nothing should have to
    go through the struct for.

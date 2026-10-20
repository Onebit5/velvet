layout
======

the tree, and what each part of it is for.

the root *is* the kernel: the subsystem directories sit at the top, the way
they do in other kernels, and beside them are the bootloader, the host
suites, the tools and the medium the kernel is booted and tested on. a tree
that builds on this one clones a release of it into a directory of its
own and sees every path here with that directory in front.

at a glance
-----------

::

    include/
      velvet/          the headers more than one subsystem reads
      uapi/velvet/     what ring 3 sees: syscall numbers, signals, keys, tty
    arch/              one directory per architecture, each with the same
                       five headers in include/asm/: irq, cpu, mmu,
                       context, machine
      x86_64/          gdt, idt, isr stubs, irq dispatch, pic, apic, port io,
                       and linker.ld, the higher-half layout
      none/            an architecture that does nothing, for portable-check
    drivers/           serial, console, tty, ps/2 keyboard and mouse, pit,
                       rtc, pci, ahci, e1000, partitions, the font
    net/               ethernet, arp, ip, icmp, udp, tcp, sockets
    fs/                vfs, disk, bcache, ext4, fat32, jbd2, path, pipe,
                       ramdisk, source, mkfs, fsck, install
    lib/               kprintf, panic, string, hash, epoch, ksyms,
                       backtrace
    mm/                pmm, buddy, slab, kmalloc, vmm, addrspace, pressure
    sched/             threads, run queue, context switch, processes,
                       signals, init, locks
    shell/             the velvet room terminal
    init/              kmain, and the handoff philemon leaves behind
    sources.mk         which of the above the kernel is made of
    boot/              philemon, the bootloader
    base/diskroot/     the files on the data disk in `make run` (input)
    tests/             the host suites, run by `make test`
    tools/             the checkers and image builders: the python ones, and
                       the C ones that replace them
      testing/selftests/  the selftests: programs that check from ring 3
                       what the kernel promises ring 3
    docs/              this
    GNUmakefile        the build, and the version at the top of it
    ROADMAP.md         where it is going
    CHANGELOG.md       where it has been

the subsystems
--------------

each kernel directory is a subsystem with a page of its own:

.. list-table::
   :header-rows: 1

   * - subsystem
     - directory
     - page
   * - the architecture boundary
     - ``arch/``
     - `subsystems/arch.rst <subsystems/arch.rst>`_
   * - memory
     - ``mm/``
     - `subsystems/mm.rst <subsystems/mm.rst>`_
   * - scheduling and processes
     - ``sched/``
     - `subsystems/sched.rst <subsystems/sched.rst>`_
   * - filesystems
     - ``fs/``
     - `subsystems/fs.rst <subsystems/fs.rst>`_
   * - drivers
     - ``drivers/``
     - `subsystems/drivers.rst <subsystems/drivers.rst>`_
   * - networking
     - ``net/``
     - `subsystems/net.rst <subsystems/net.rst>`_
   * - the library
     - ``lib/``
     - `subsystems/lib.rst <subsystems/lib.rst>`_
   * - the shell
     - ``shell/``
     - `subsystems/shell.rst <subsystems/shell.rst>`_
   * - the bootloader
     - ``boot/``
     - `subsystems/boot.rst <subsystems/boot.rst>`_
   * - the checkers and image builders
     - ``tools/``
     - `subsystems/tools.rst <subsystems/tools.rst>`_

what the kernel is made of
--------------------------

``sources.mk`` names the files and directories the portable kernel is
compiled from, and the build adds exactly one directory under ``arch/``.
the root holds more than the kernel, ``boot/``, ``tests/`` and ``tools/``
among it, so nothing globs the root: the makefile, ``tools/checkarch.py``,
``tools/portable.py`` and ``tools/toolcheck.py`` all read that one list, and
so can a build of the kernel from outside it. a new file in an existing directory
costs no edit anywhere; a new directory is one word in ``sources.mk``.

headers
-------

where a header lives says who may read it.
``include/velvet/`` holds every header another subsystem uses, included as
``<velvet/vfs.h>``. ``arch/<arch>/include/asm/`` holds one architecture's,
included as ``<asm/cpu.h>``, and the build puts exactly one of those on
the include path. ``include/uapi/velvet/`` is the part of the kernel's
interface that ring 3 compiles against too, the syscall numbers, signal
numbers, keys and terminal flags, and includes nothing else of the
kernel's. a header only its own subsystem reads stays beside its code.
``<generated/version.h>`` is written into the object tree by the build,
from the version at the top of the ``GNUmakefile``. the rules are in
`code style <coding-style.rst>`_.

the one rule that crosses all of them
-------------------------------------

**machine-specific code lives under ``arch/<machine>/`` and nowhere else.**
everything above that line is portable, and ``make portable-check`` and
``tools/checkarch.py`` fail the build when something crosses it. the full
account is in `the architecture boundary <subsystems/arch.rst>`_ and in
`code style <coding-style.rst>`_.

inputs and outputs
------------------

``base/diskroot/`` is an *input*, copied onto the data disk by ``make
run``. the boot tar the loader hands the kernel has no input directory: it
is ``/init`` and the selftests under ``selftests/``, built from
``tools/testing/selftests/``, and nothing else. it is the smallest medium
the kernel can be booted and tested on, and a system built on the kernel
packs its own.

everything under ``bin/`` and ``obj/`` is generated and comes back out with
``make clean``. the boot image, ``velvet.img``, is written by
``tools/mkboot.py`` and carries philemon, the kernel, the ramdisk and the
kernel's own source tree. `building`_ is the details, including how another
tree builds the kernel into its own ``bin/`` instead.

.. _building: building.rst

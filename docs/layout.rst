layout
======

the tree, and what each part of it is for.

the root *is* the kernel: the subsystem directories sit at the top, the way
they do in other kernels, and beside them are the bootloader, the host
suites, the tools and the medium the kernel is booted and tested on. a tree
that carries this one as a submodule, which is how velvetOS has it under
``kernel/``, sees every path here with that one directory in front.

at a glance
-----------

::

    arch/              the boundary: irq, cpu, mmu, context, machine
      x86_64/          gdt, idt, isr stubs, irq dispatch, pic, apic, port io
      none/            an architecture that does nothing, for portable-check
    drivers/           serial, console, tty, ps/2 keyboard and mouse, pit,
                       rtc, pci, ahci, e1000, partitions, the font
    net/               ethernet, arp, ip, icmp, udp, tcp, sockets, dhcp,
                       dns, http
    fs/                vfs, disk, bcache, ext4, fat32, jbd2, path, pipe,
                       ramdisk, source, mkfs, fsck, install
    lib/               kprintf, panic, string, hash, deflate/inflate,
                       epoch, ksyms, backtrace
    mm/                pmm, buddy, slab, kmalloc, vmm, addrspace, pressure
    sched/             threads, run queue, context switch, processes,
                       signals, auth, init, locks
    shell/             the velvet room terminal
    main.c             kmain
    boot.c, boot.h     the loader handoff
    version.h          the kernel's version
    linker.ld          the higher-half layout and the section symbols
    sources.mk         which of the above the kernel is made of
    boot/              philemon, the bootloader
    base/ramdisk/      the files packed into the boot tar (input)
    base/diskroot/     the files on the data disk in `make run` (input)
    tests/             the host suites, run by `make test`
    tools/             the checkers and image builders: the python ones, and
                       the C ones that replace them
    docs/              this
    GNUmakefile        the build
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
so does velvetOS's self-hosting build. a new file in an existing directory
costs no edit anywhere; a new directory is one word in ``sources.mk``.

the one rule that crosses all of them
-------------------------------------

**machine-specific code lives under ``arch/<machine>/`` and nowhere else.**
everything above that line is portable, and ``make portable-check`` and
``tools/checkarch.py`` fail the build when something crosses it. the full
account is in `the architecture boundary <subsystems/arch.rst>`_ and in
`code style <coding-style.rst>`_.

inputs and outputs
------------------

``base/ramdisk/`` and ``base/diskroot/`` are *inputs*: the first is packed
into the boot tar the loader hands the kernel, the second is copied onto the
data disk by ``make run``. the ramdisk here carries no programs. it is the
smallest medium the kernel can be booted and tested on: the accounts, the
profile the shell reads at login, and the files the boot test reads back. a
system built on the kernel packs its own.

everything under ``bin/`` and ``obj/`` is generated and comes back out with
``make clean``. the boot image, ``velvet.img``, is written by
``tools/mkboot.py`` and carries philemon, the kernel, the ramdisk and the
kernel's own source tree. `building`_ is the details, including how another
tree builds the kernel into its own ``bin/`` instead.

.. _building: building.rst

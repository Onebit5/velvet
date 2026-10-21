building
========

velvet builds with a normal toolchain and boots in qemu. there is no
configure step and no dependencies to fetch beyond a handful of programs.

requirements
------------

- ``gcc``, the machine's compiler, for the kernel, the host suites and the
  C tools, and for philemon's 64-bit half
- ``nasm``, for the ``.asm`` sources under ``arch/x86_64/``, and for
  philemon's first stage
- ``git``, which fetches philemon the first time an image is built
- ``make``. GNU make, for the ``GNUmakefile``
- ``python3``, for the checks and image builders in ``tools/``. no
  third-party packages
- ``qemu-system-x86_64``, only for ``make run`` and ``make boottest``

on fedora:

.. code-block:: sh

   dnf install gcc nasm make python3 qemu-system-x86

on debian or ubuntu:

.. code-block:: sh

   apt-get install gcc nasm make python3 qemu-system-x86

the targets
-----------

``make``
   the default target: ``velvet.img``, a bootable disk image
   philemon wrote, with the kernel and a ramdisk that carries no programs.
   the kernel elf lands in ``bin/velvet`` alongside it.

``make kernel``
   just the kernel elf, ``bin/velvet``.

``make philemon``
   the loader, fetched and built: ``bin/boot/philemon.bin`` and
   ``bin/boot/philemon64.bin``. philemon is a project of his own, and
   ``philemon-release.mk`` names the release of him the image is written
   with. ``tools/philemon.py`` clones that release's tag into
   ``obj/philemon/``, once, and his own makefile builds him from there.
   ``PHILEMON_URL=../philemon`` clones the same release out of a local
   copy, and ``PHILEMON_SRC=../philemon`` builds a checkout of him as it
   is, which is how the two are worked on together.

``make tools``
   the C build tools, into ``bin/tests/``: ``mkext4``, ``mkfat``,
   ``gensyms``, ``bin2c``, ``checkfmt`` and ``fsckext4``.

``make kernelrelease``
   print the version: ``VERSION``, ``PATCHLEVEL`` and ``SUBLEVEL`` from the
   top of the ``GNUmakefile``, which is the one place it is written. see
   `releases <versioning.rst>`_.

``make run``
   boot ``velvet.img`` in qemu with a second drive carrying the data
   filesystem. defaults are four cpus and 2 GiB:

   .. code-block:: sh

      make run                 # four cpus, 2G
      make run CPUS=1          # one cpu, which must always still work
      make run MEM=512M        # less memory, for the pressure paths
      make run QEMU_EXTRA="-d int"

   qemu's window opens *and* com1 is wired to your terminal, and both feed
   the same input queue. the shell cannot tell the difference. tcp port
   5555 and udp port 5556 are forwarded to the host, so the machine can be
   reached from outside; the forwarding is the *only* way in, so from another
   terminal it is ``nc -u localhost 5556`` and not the guest's own address.

   the image asks which selftests to run, and ``none`` leaves the machine
   up. alt+5, or ``ctrl+\`` then 5 on serial, is the kernel's monitor,
   where its builtins all work. it runs no programs, and booting the kernel
   with programs on it is what a system built on it is for.

``make disk``
   throw ``disk.img`` away and build a fresh one. ``make run`` builds it once
   and then leaves it alone, because what you write to it is meant to stay
   written.

``make lint``
   the style guide, as much of it as a machine can check: the header, the
   comment shape, the function brace, dashes and first person, tabs, markers.
   the whole tree by default, or ``make lint FILES='fs/ext4.c'`` for the
   files you name. it is the first thing CI runs, because it needs nothing
   built.

``make test``
   the host test suites and every checker. no qemu, about a minute. this is
   the same command CI runs first, and `testing`_ is the full account.

``make boottest``
   build the image, boot it headless, and type commands at the shell over
   the serial port, checking the answers. needs qemu. the serial log is
   written to ``bin/*-boottest.log``, which is what CI uploads when the
   run fails.

``make fuzz``
   the parsers that read from the wire and from disks, fed random and
   mutated input under ubsan. see `testing`_.

``make clean``
   remove ``bin/`` and ``obj/``.

the architecture knobs
----------------------

``ARCH`` selects the architecture and defaults to ``x86_64``, the only one
there is. the mechanism for more than one is kept deliberately. an
aarch64 port was written and removed, and the boundary it forced is most of
why ``arch/`` is worth anything. see `the architecture boundary
<subsystems/arch.rst>`_.

``make portable-check``
   compile the portable half of the kernel. ``mm/``, ``sched/``,
   ``fs/``, ``lib/``, most of ``drivers/``, against ``arch/none/``, an
   architecture whose every function is an empty stub. if it compiles,
   nothing above the line needs a machine. add ``LIST=1`` and the undefined
   symbols that come back are the porting checklist: every name a port
   would have to define.

``make arch-check ARCH=x86_64``
   compile ``arch/<ARCH>/`` and nothing else. this is what can honestly be
   said about an architecture with no boot code yet. ``CROSS=`` sets the
   toolchain prefix for a cross build.

building it from another tree
-----------------------------

a tree that ships this kernel builds a release of it with this makefile,
from the outside, wherever it cloned it to:

.. code-block:: sh

   make -C path/to/velvet BINDIR=$PWD/bin/kernel OBJROOT=$PWD/obj/kernel \
        kernel tools

``BINDIR`` and ``OBJROOT``
   where the outputs go. the defaults are ``bin`` and ``obj`` here; another
   tree points them into its own build directory, so a build there leaves
   nothing in this checkout.

``sources.mk``
   what the kernel is made of. a build that compiles the kernel with other
   tools, an assembler and a linker of its own for instance, includes this
   file rather than keeping a list of its own.

the include path and the version
   the same build needs the kernel's include path, ``include/`` and
   ``arch/x86_64/include/``, and a ``generated/version.h`` of its own on
   it, defining ``VELVET_VERSION`` from the three lines at the top of the
   ``GNUmakefile``. those lines are the first three of the file, in that
   order, so they can be read without being make.

``$(OBJROOT)/x86_64/kernel.objects``
   every object the kernel was linked from, in link order, written beside
   the kernel at each link, so another linker can be handed exactly the same
   inputs.

the suites and the python checkers find their images under ``bin/tests/``,
so ``make test`` is run here with ``BINDIR`` left alone.

the build tree
--------------

everything generated comes back out with ``make clean``:

``bin/``
   the kernel, the ramdisk, the test binaries and the C tools, and in
   ``bin/boot/`` the loader.

``obj/``
   one object tree per architecture. ``obj/x86_64/``, so switching
   ``ARCH`` and rebuilding does not link x86 objects into another
   machine's kernel and report a relocation error rather than what you did.
   and ``obj/include/generated/version.h``, which the build writes from the
   top of the ``GNUmakefile`` and rewrites only when the version changes.

``velvet.img``
   the bootable image: philemon and its second-stage loader, the kernel and
   the ramdisk tar.

``disk.img``
   the data disk ``make run`` attaches.

``base/diskroot/`` is input, not output: the files that go onto the data
disk in ``make run`` come from it. the tar the loader hands the kernel has
no input directory of its own; it is ``bin/init`` and ``bin/selftests/``,
packed.

notes
-----

- the kernel builds ``-Wall -Wextra`` and takes both seriously. a warning
  nobody can justify is a bug that has not happened yet.
- there is no ``make iso`` target any more. the image is written by
  philemon's ``tools/mkboot.py`` and boots directly; the old iso went away
  in 0.1.12, when philemon replaced the bootloader before it.

.. _testing: testing.rst

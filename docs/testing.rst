testing
=======

most of this kernel can be tested without booting anything, and that is a
design decision rather than a happy accident. the parts that *think* are
kept separate from the parts that *touch hardware*, so the suites compile
the real kernel sources as ordinary linux programs and poke at them.

every behaviour change arrives with the suite that proves it. see `tests
come with the change <contributing.rst>`_ in the contributing guide.

how it works
------------

a function that would otherwise reach for the machine is written to take
what it needs as an argument:

- ``pmm_init_from_map()`` takes a memory map instead of asking the loader
  for one, so a test can fabricate one over a ``malloc``'d arena.
- ``keyboard_feed()`` takes a scancode instead of reading port 0x60.
- ``serial_feed()`` takes a byte.
- ``run_line()`` takes a string.

host builds define ``VELVET_HOSTED``, which turns ``irq_save()`` /
``irq_restore()`` into no-ops. userspace is not allowed to ``cli`` and has
nothing to lock out anyway. that is what lets the real spinlocks,
allocators and scheduler run in a test process. see `testability
<coding-style.rst>`_ for the rule that keeps this seam alive.

``make test``
-------------

the entry point, and the first thing CI runs. no qemu, about a minute:

.. code-block:: sh

   make test

it builds and runs three kinds of thing.

the suites
~~~~~~~~~~

the suites in ``tests/test_*.c``, linked against the real kernel sources and
run as host programs. each prints ``ok`` or ``FAILED`` and a failure prints
the failing case. all of them:

``kprintf``, ``ansi``, ``termios``, ``path``
   formatting, terminal escape sequences, the terminal discipline's
   decisions, and path resolution.

``mm``, ``buddy``, ``slab``, ``vmm``, ``addrspace``, ``pressure``
   the allocators and the page tables, including draining ram dry, every
   frame handed out exactly once, and a leak-free teardown.

``locks``, ``switch``, ``process``, ``syscall``, ``signal``, ``init``
   eight real threads through the real allocators at once; a real context
   switch in userspace; pids, zombies, file descriptors and a kill that
   races; the syscall layer refusing every bad pointer; signals; and what
   init decides about a service that keeps dying.

``gdt``, ``rtc``, ``acpi``, ``pci``, ``console``, ``keyboard``, ``serial``, ``mouse``, ``ksyms``, ``epoch``
   the x86 structures and the drivers, fed fabricated input. a machine's
   worth of firmware tables and every malformed one refused.

``ramdisk``, ``elf``, ``vfs``, ``fat32``, ``bcache``, ``ext4``, ``part``, ``mkfs``, ``source``, ``pipe``, ``crash``, ``resize``
   the filesystems. a real image with long names and subdirectories, the
   block cache's eviction and write-back, inodes and bitmaps and the
   indirect block map, mbr and gpt, and the archive on raw sectors. the
   ``crash`` and ``resize`` suites stop the machine at every write one of
   those operations takes and demand a filesystem at each one.

``net``, ``tcp``
   checksums, headers, sockets and connections, checked against packets
   captured from the rfcs rather than invented here.

``shell``, ``tty``, ``install``, ``philemon``, ``hash``
   the shell's parsing and dispatch, the terminal discipline, the
   installer's ordering, the loader's elf parsing, and the two checksums.

the ``shell`` suite mounts ``bin/ramdisk.tar``, the medium the build just
made, and completes names against what is really on it.

the foreign-implementation checks
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

a check written by the code it checks proves nothing. where another
implementation exists, the project tests against that instead:

``hashcheck``
   sha-1 and crc32 against python, ``zlib`` and git itself.

``fsckcheck`` / ``fsck`` / ``mkfsck`` / ``growcheck``
   break a filesystem on purpose and demand this project's fsck finds it;
   then read the repaired image back with ``tools/readext4.py``, which
   shares no line with the checker.

``srccheck``
   read the source tree back out of the boot image and hold it against the
   working tree, and require the archive's sha-1 to appear inside the kernel
   shipped beside it.

the static checks
~~~~~~~~~~~~~~~~~

``lint``
   the style guide, checked mechanically: the file header, the comment shape,
   the function brace, dashes and first person in prose, tabs and the
   ``//`` rule. ``make lint`` covers the tree and
   ``make lint FILES='fs/ext4.c'`` covers the files named. naming prefixes are
   warnings until the tree is converted. it runs before anything is built, on
   its own and in ci, so it is covered here rather than under the suites.

``checkfmt``
   every ``kprintf``/``panic`` format string against what ``lib/kprintf.c``
   actually implements. this exists because of a bug that cost an
   afternoon: gcc checks a format string against *real* printf, so it
   accepted a ``%-7s`` this formatter never implemented, which printed
   itself literally and read every later argument from the wrong slot.

``checkarch``
   no inline assembly and no x86 headers outside ``arch/``.

``portable-check``
   the portable half of the kernel compiled against ``arch/none/``.

``toolcheck``
   the C build tools against the python ones they replace.

``boottest``
------------

the one that needs a machine:

.. code-block:: sh

   make boottest

it builds the image, boots it headless in qemu, tells its ``/init`` to
run no selftests, and **types commands at the kernel's monitor over the
serial port**, checking the answers. that is only possible because com1
feeds the same input queue the keyboard does. the serial log is left in
``bin/*-boottest.log``, and CI uploads it, with the boot image, when a
run fails.

what it checks is what the kernel can show with no system on its medium:
the boot itself, the memory map and the allocators, both cores, the
scheduler, ``/init`` asking and the monitor answering, every builtin it
types, both disks mounted, the journal started, a ``resize`` that grows
the work filesystem into its partition, ``fsck`` finding nothing twice
running, and the source on the medium verified against the stamp and
unpacked onto the disk. ring 3 is the other half of the machine: the
selftests below check what the kernel promises it, and the programs a
system ships are that system's to check.

``kselftest``
-------------

the kernel's selftests: programs in ``tools/testing/selftests/`` that
check from ring 3 what the kernel promises ring 3, built against the uapi
headers alone and run on the booted image as guest.

.. code-block:: sh

   make kselftest                  # every test
   make kselftest TARGETS=sched    # the tests of one subsystem

`kernel selftests <dev-tools/kselftest.rst>`_ is the whole of it: running
them, writing one, the ``kselftest.h`` calls, and the rule a failing one is
read by, which is that the kernel does not break userspace.

running one suite
-----------------

each suite is an ordinary binary in ``bin/tests/``:

.. code-block:: sh

   make bin/tests/tcp
   ./bin/tests/tcp

``make test`` is still the thing to run before pushing, because it also
runs the static checks and the foreign-implementation comparisons, and a
suite passing on its own does not mean the tree is clean.

what CI does with all of this
-----------------------------

the workflow runs, in order: ``make test``, ``make``, ``make boottest``,
``make kselftest``.
see `ci is the gate <contributing.rst>`_ for what happens when one of them
fails, which is: the pull request does not merge.

the smoke test
--------------

`contributing`_ covers the pre-push sequence. for a hand check of a running
machine, ``make run`` gives you a shell on com1 and on the qemu window. the
version-specific things worth poking after each change are listed by version
in `CHANGELOG.md <../CHANGELOG.md>`_. each entry is a machine that booted,
and its sentence is what to try.

.. _contributing: contributing.rst

the analysers, and what they are worth here
-------------------------------------------

``sparse`` runs over the kernel and is clean apart from four kinds of
warning, every one of which is deliberate:

- a memset of a whole cache or scroll buffer, which is what it is for
- casts that truncate a constant, in the big-endian helpers, where the
  truncation *is* the byte swap

the one real report is a cast of a compound literal to its own type in
``drivers/tty.c``, which is legal and pointless, and is tagged as such.

``smatch`` is installed and reports nothing at all, including on a file
written to dereference a null pointer on purpose, so it is not evidence of
anything and should not be listed as a check until somebody works out why.

``ubsan`` works in trap mode, which needs no runtime:

    make HOSTFLAGS="-std=gnu11 -Wall -Wextra -Wno-stringop-truncation \
        -Wno-stringop-overread -g -DVELVET_HOSTED -I. -Iinclude \
        -Iarch/x86_64/include -Iobj/include -DVELVET_ARCH_X86_64 \
        -fsanitize=undefined -fsanitize-trap=undefined \
        -fno-sanitize-recover=all" <the suites>

two traps for the unwary: make will not rebuild binaries that already exist,
so they have to be removed first or the flags never reach the compiler, and
the suite list has to be taken from ``TEST_BINS`` without the trailing colon
that the continuation puts on the last one. a ub built with trap mode
contains ``ud2`` instructions, and that is the way to tell it worked:

    objdump -d bin/tests/mm | grep -c ud2

the fuzzer
----------

``make fuzz`` builds ``tools/fuzz_parsers.c`` with ubsan in trap mode and
feeds random bytes, and random mutations of a valid seed, to the two parsers
that take input from the wire or from a disk:

- ``tcp_parse``, including lengths shorter than the header it is reading
- ``ustar_octal``, with fields of the wrong length and a size that never ends

two hundred thousand rounds of each takes about a second and a half. The seed is
fixed, so a run that finds something can be repeated exactly:

    bin/tests/fuzz_parsers 0x<seed>

As it stands both survive it: no crash, no hang and no trap.

It checks its seeds before it starts and refuses to run if one is refused,
which is not decoration: the first version of it wrote a tcp segment out by
hand with zeros in the checksum, so tcp_parse refused every input it was
ever given and the clean result was about nothing. A fuzzer that reports a
clean run over input the parser never reads is worse than no fuzzer, because
it is believed.

ubsan on the kernel itself
--------------------------

The suites are instrumented in the host build, which covers the code the
suites reach. The kernel can be instrumented too, and that is the only way
anything in ring 0, on the trap path, in a driver or on a second core is
checked at all. The flags go in through ``CFLAGS``, which has to be given in
full, because a value on the command line replaces the makefile's rather than
adding to it:

    make CFLAGS="<the flags the makefile uses> -fsanitize=undefined \
        -fsanitize-trap=undefined -fno-sanitize-recover=all"

Trap mode is what makes this possible in a freestanding kernel: every check
becomes a ``ud2`` instead of a call into a runtime that does not exist here.
So the way to tell the build worked is the same as for the suites:

    objdump -d bin/velvet | grep -c ud2

At 0.4.0 that was 13275 checks, and the boot test of the time, 95 checks
over the kernel and the programs it then shipped with, passed with them armed, on
one cpu and on two: the boot, the memory map, paging, both cores, the trap
path, the disk and keyboard drivers, the allocators, the filesystems,
syscalls, ring 3, signals and the shell.

What this does not cover, and it is worth knowing why: a read past the end of
a buffer that sits inside a larger allocation is not undefined behaviour as
the language sees it, so ubsan cannot see the dirent over-read that
``fs/ext4.c`` records as a fixme. That class needs asan, which has no usable
runtime on this machine, or a fuzzer aimed at the function, which is what
``make fuzz`` is for.

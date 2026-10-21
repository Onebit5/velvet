subsystems
==========

one page per subsystem: what ``<name>/`` is for, what it decided, and what
it deliberately does not do. the map of the whole tree is in `layout
<../layout.rst>`_.

- `the architecture boundary <arch.rst>`_. ``arch/``
- `the boot protocol <boot.rst>`_. ``include/velvet/boot.h``
- `memory <mm.rst>`_. ``mm/``
- `scheduling and processes <sched.rst>`_. ``sched/``
- `filesystems <fs.rst>`_. ``fs/``
- `drivers <drivers.rst>`_. ``drivers/``
- `networking <net.rst>`_. ``net/``
- `the library <lib.rst>`_. ``lib/``
- `the tools <tools.rst>`_. ``tools/``

each page is written to be read on its own, but they do lean on each other:
the arch boundary comes up in memory and scheduling, the filesystem pages
reference the block cache and the journal, and the syscall layer described
in scheduling and processes is the other half of `the system calls
<../syscalls.rst>`_.

what runs on top of the kernel is not here: a libc, programs, and the tools
that build them belong to the system built on it.

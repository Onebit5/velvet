docs
====

velvet is a tiny 64-bit kernel for x86_64, written in C, booted by a
bootloader of its own. this is its documentation: what each part is, why it
is the way it is, and how to work on it.

it is the kernel of velvetOS, and until 0.4.0 the two were one tree. the
system built on top of it, the libc, the programs, the assembler, linker and
make that build this kernel, and git, is velvetOS, which carries this
repository as its ``kernel/`` submodule.

the documentation is split in two. the pages here at the root are *about the
project*: how to build it, how to test it, how to write and send a change.
the pages under `subsystems/ <subsystems/index.rst>`_ are *about the code*.
one page per subsystem, saying what it is for and what it decided.

the short version of the machine is in `README.md <../README.md>`_; where it
is going is `ROADMAP.md <../ROADMAP.md>`_, and where it has been is
`CHANGELOG.md <../CHANGELOG.md>`_.

start here
----------

if you are building it for the first time: `building <building.rst>`_.

if you are changing it: `code style <coding-style.rst>`_, then `testing
<testing.rst>`_, then `contributing <contributing.rst>`_.

if you are reading it: `layout <layout.rst>`_ for the map, then the
subsystem page you care about.

about the project
-----------------

- `code style <coding-style.rst>`_: how to write the code, name things,
  comment, and handle errors; the TODO/FIXME convention.
- `contributing <contributing.rst>`_: the workflow, commit messages,
  sign-off, review, patches by email, and the rule that CI is the gate.
- `building <building.rst>`_: the toolchain, every make target, running
  it, and building it from inside another tree.
- `testing <testing.rst>`_: the host suites, the checks against other
  implementations, the boot test, and what CI runs.
- `layout <layout.rst>`_: the source tree and the subsystem map.
- `releases <versioning.rst>`_: how versions are cut, and why they are not
  in the log.

the subsystems
--------------

- `the architecture boundary <subsystems/arch.rst>`_: ``arch/``, and the
  one rule the build enforces.
- `philemon <subsystems/boot.rst>`_: the bootloader and the handoff.
- `memory <subsystems/mm.rst>`_: frames, heap, page tables, demand paging,
  copy on write, pressure.
- `scheduling and processes <subsystems/sched.rst>`_: threads, the
  scheduler, locks, processes, signals, init.
- `filesystems <subsystems/fs.rst>`_: the namespace, the ramdisk, fat32,
  ext4, the journal, the block cache, partitions, pipes.
- `drivers <subsystems/drivers.rst>`_: console, serial, terminal, keyboard,
  mouse, timers, pci, sata, partitions.
- `networking <subsystems/net.rst>`_: ethernet through tcp, sockets, dhcp,
  dns, http.
- `the library <subsystems/lib.rst>`_: printing, strings, checksums,
  deflate, dates, symbols.
- `the shell <subsystems/shell.rst>`_: the velvet room terminal.
- `the system calls <syscalls.rst>`_: the abi between ring 3 and the kernel,
  every call with the arguments it takes and what it gives back.
- `the tools <subsystems/tools.rst>`_: the checkers and image builders.

other files
-----------

- `README.md <../README.md>`_. what the kernel is, in one screen.
- `ROADMAP.md <../ROADMAP.md>`_. what it will be, version by version.
- `CHANGELOG.md <../CHANGELOG.md>`_. what it has been.
- `LICENSE <../LICENSE>`_ and `FONT-LICENSE <../FONT-LICENSE>`_.
- `sources.mk <../sources.mk>`_. what the kernel is made of, for any build.
- `.github/workflows/ci.yml <../.github/workflows/ci.yml>`_. what CI runs.

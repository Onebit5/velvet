kernel selftests
================

the kernel carries a set of selftests under ``tools/testing/selftests/``.
each one is a small program that exercises one path through the kernel
from ring 3, and checks what comes back against what the kernel promises a
program. they run on a booted kernel, the way any program does.

a selftest sees the kernel from where a program does. it is an ordinary
static program, built with musl, a libc that has never heard of this
kernel and calls it the way it calls any kernel on x86_64. so it can rely
on the abi and on nothing behind it, and every libc call it makes is a
check that the abi is the one programs expect. that is what makes the
selftests the abi's own record, and why a failing one is read the way
`what a failing selftest means`_ says.

running the selftests
---------------------

to build the tests and run them:

.. code-block:: sh

   make kselftest

there is nothing to install. the tests are built with the image, which
carries them under ``selftests/``, and its ``/init`` is what runs them. so
``make kselftest`` builds whatever is out of date, boots the image headless
in qemu, answers the question ``/init`` asks with the tests to run, and
reads the results back off the serial port. ``/init`` says when each test
starts and how it ended, and powers the machine off after the last.

beyond the build it needs qemu and ``musl-gcc``: from the distribution,
``musl-tools`` on debian and ubuntu and ``musl-gcc`` on fedora, or from a
musl built anywhere, named with ``MUSL_GCC``:

.. code-block:: sh

   make kselftest MUSL_GCC=$HOME/musl/bin/musl-gcc

the results are one TAP 13 stream, each test's own TAP under it as ``#``
lines:

.. code-block:: text

   TAP version 13
   1..1
   # timeout set to 30
   # selftests: sched: wait
   # TAP version 13
   # 1..7
   # ok 1 exit(42) reaches the parent as an exit with 42
   # ok 2 WNOHANG says nothing has happened yet
   # ok 3 a child ended by SIGTERM reaches it as that signal
   # ok 4 wait refuses a pid that is not the caller's child
   # ok 5 SIGCHLD under SA_RESTART leaves the wait whole
   # ok 6 a child of a parent ignoring SIGCHLD is collected for it
   # ok 7 and with none left, any child is none
   # # Totals: pass:7 fail:0 xfail:0 xpass:0 skip:0 error:0
   ok 1 selftests: sched: wait
   # Totals: pass:1 fail:0

``make kselftest`` fails when any test does. the whole serial log of the
run, everything the kernel printed included, is kept in
``bin/velvet-kselftest.log``.

the tests run as guest, uid 1000, the account the kernel does the least
for. ``/init`` puts each in a group of its own, in front of the console,
and becomes guest in it before the test starts. a test can do only what
any logged-in user could, and a promise the kernel keeps to a guest it
keeps to everybody. before the first test ``/init`` makes ``/tmp`` on the
disk, mode 1777, as somewhere a guest may make files of its own.

a test of what only root may do, reading a drive by the byte for one, is
the exception, and the file says so rather than the test: the ones named
in ``SELFTEST_ROOT`` in the GNUmakefile go on the medium mode 0700, which
the guest could not run at all, and ``/init`` runs a test nobody else may
run as root. it says so before the test starts::

   init: selftests/fs/blockdev is root's, and runs as root

the machine has a second drive: a copy of ``disk.img``, the disk ``make
run`` boots, made fresh for each run and thrown away after it, so a test
that writes leaves nothing for the next run to trip on, and a call about
filesystems has real ones to answer about. before it asks anything,
``/init`` does what a system's does: it mounts ``/proc``, mounts the
drive's first filesystem, moves ``/dev`` and ``/proc`` onto it and makes
it the root with ``pivot_root``, the archive it came from going to
``/boot``, and mounts the work filesystem, which is smaller than its
partition, at ``/work``. the tests are then under ``/boot/selftests``,
and with no disk they stay where they were, under ``/selftests``.

running a subset of selftests
-----------------------------

``TARGETS`` names the subsystems to run, by directory:

.. code-block:: sh

   make kselftest TARGETS=sched
   make kselftest TARGETS="sched x86_64"

without it every directory under ``tools/testing/selftests/`` is a target,
so the directories there are the list of targets.

the runner can also be called on its own, on an image this tree built,
with the tests named one by one:

.. code-block:: sh

   python3 tools/testing/selftests/run_kselftest.py velvet.img sched/wait
   python3 tools/testing/selftests/run_kselftest.py --timeout 60 velvet.img x86_64/fault
   python3 tools/testing/selftests/run_kselftest.py --disk disk.img velvet.img fs/sync

without ``--disk`` the machine has the boot medium and nothing else.

the machine has 2 cpus unless ``CPUS`` says otherwise:
``make kselftest CPUS=4``.

running a test by hand
----------------------

a selftest is an ordinary program on the medium. boot with ``make run``
and answer ``/init`` with its name:

.. code-block:: text

   run which selftests? sched/wait

``all`` is every test, and ``timeout=SECONDS`` gives each longer than 30.
its TAP goes to the screen between whatever the kernel prints while it
runs, which is often the part worth reading. ``none`` runs nothing and
powers the machine off, which is the boot test's answer.

timeouts and panics
-------------------

selftests are meant to be quick, a few seconds each, and every test gets
30. one that has not come back by then is reported as

.. code-block:: text

   not ok 1 selftests: x86_64: fault # TIMEOUT 30 seconds

``/init`` kills it, and its whole group with it, and goes on to the next.
``--timeout`` gives the runner another limit while a test is being worked
on; for the tests in the tree it is 30.

a test that takes the kernel down is reported as ``# kernel panic``, with
the first lines of the panic under it, and every test after it as not run,
since there is no kernel left to run them on.

contributing new tests
----------------------

in general, the rules for selftests are:

- test what a program can see. a test includes ``kselftest.h`` and the
  libc's headers, and nothing from inside the kernel.
- do everything as guest, unless what is under test is what only root
  may do. then name the test in ``SELFTEST_ROOT``, and have it check, by
  becoming guest in a child, that a guest is refused.
- don't take long: a few seconds, and never near the 30.
- leave nothing behind: wait for every child the test forks, and end with
  ``ksft_finished()``.
- report in TAP, through ``kselftest.h`` and never by hand, so the runner
  and CI read every result the same way.
- a bug a program can see comes with a selftest that fails before the fix
  and passes after it. it lands with the fix or after it, never before,
  so no commit in the history fails its own selftests and a bisect can
  trust every one of them.

contributing new tests (details)
--------------------------------

- a test is one ``.c`` file in the directory of the subsystem it tests,
  ``tools/testing/selftests/<subsystem>/<test>.c``. nothing lists it: the
  build finds every ``.c`` there, compiles it to
  ``bin/selftests/<subsystem>/<test>``, and packs it on the medium as
  ``selftests/<subsystem>/<test>``. a new directory is a new target.
  ``init.c``, above the directories, is not a test but the ``/init`` that
  runs them, built the same way.
- a test defines ``main`` and includes ``../kselftest.h``. it ends with
  ``ksft_finished()``, which exits with the right status.
- it is linked static against musl, so the whole of a libc is there. its
  results go through ``kselftest.h`` rather than stdio, which writes each
  line with one ``write``: a test forks, and a buffer a child inherits
  would be printed twice.
- a result's name says what the kernel promises, not what the test did:
  ``exit(42) reaches the parent as 42``, rather than ``test exit``.
- a diagnostic goes before the result it explains, through
  ``ksft_print_msg()``, and says what was wanted and what was read.
- a test is C like the rest of the tree, and ``make lint`` holds it to the
  `code style <../coding-style.rst>`_.

kselftest.h
-----------

reporting
~~~~~~~~~

``ksft_print_header()``
   prints ``TAP version 13``, the first thing a test prints.

``ksft_set_plan(n)``
   prints ``1..n``: how many results will follow.

``ksft_test_result(ok, fmt, ...)``
   one result, numbered from 1: ``ok n`` or ``not ok n``, then the name
   ``fmt`` makes, which ends with a newline.

``ksft_print_msg(fmt, ...)``
   a diagnostic, printed after a ``#`` so no reader takes it for a result.

``ksft_finished()``
   prints the totals and exits: ``KSFT_PASS`` when every planned result
   ran and was ok, ``KSFT_FAIL`` otherwise, with a note when the number
   run is not the number planned.

the formats are printf's. each line is built whole and written with one
call, so it reaches the serial port whole even when the kernel prints
something at the same moment.

``KSFT_PASS`` (0), ``KSFT_FAIL`` (1) and ``KSFT_SKIP`` (4) are the exit
codes. nothing skips yet.

helpers
~~~~~~~

everything else a test does is the libc's: ``fork``, ``waitpid``,
``kill``, ``sigaction``, ``open``, ``mmap`` and the rest, under the names
every program uses. a call the libc has no wrapper for is
``syscall(SYS_name, ...)``, by the numbers in ``<sys/syscall.h>``. beside
them, ``kselftest.h`` has the few things tests kept writing:

``ksft_streq(a, b)``
   whether two strings are the same.

``ksft_read_file(path, buf, size)``
   reads a whole file, up to ``size`` bytes, and returns its length, or
   -1 when it would not open.

``ksft_contains(text, len, needle)``
   whether ``needle`` is somewhere in the first ``len`` bytes of
   ``text``. with ``ksft_read_file``, a check of what a file in ``/proc``
   says.

``ksft_sleep_ms(ms)``
   a pause that a signal does not cut short.

``ksft_exited(status, code)`` and ``ksft_killed_by(status, sig)``
   whether a wait status says the child exited with ``code``, or was
   ended by ``sig``.

example
-------

a whole test, with one result:

.. code-block:: c

   // SPDX-License-Identifier: GPL-2.0-only
   /*
    * tools/testing/selftests/sched/exit.c
    * Your Name <you@example.com>, 2026
    * what a child passes to exit is what its parent reads.
    */

   #include "../kselftest.h"

   int main(void)
   {
       int i_status = -1;

       ksft_print_header();
       ksft_set_plan(1);

       pid_t i_pid = fork();
       if (i_pid == 0) {
           _exit(7);
       }
       waitpid(i_pid, &i_status, 0);
       if (!ksft_exited(i_status, 7)) {
           ksft_print_msg("wanted an exit with 7, read 0x%x\n", i_status);
       }
       ksft_test_result(ksft_exited(i_status, 7),
                        "exit(7) reaches the parent as an exit with 7\n");
       ksft_finished();
   }

saved as ``tools/testing/selftests/sched/exit.c``, it runs with the rest of
``sched``:

.. code-block:: sh

   make kselftest TARGETS=sched

what a failing selftest means
-----------------------------

a selftest is a promise the kernel has made to programs, written down
where a machine can check it. so when a change turns one from ``ok`` to
``not ok``, the change is what is wrong, not the test: **the kernel does
not break userspace.** that holds even when the old behaviour looks like
an accident, because some program already depends on it.

a selftest changes only when the promise does. that is a decision about
the abi, made on purpose: the commit that makes it says so, and says what
programs have to do about it.

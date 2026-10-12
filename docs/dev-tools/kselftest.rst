kernel selftests
================

the kernel carries a set of selftests under ``tools/testing/selftests/``.
each one is a small program that exercises one path through the kernel
from ring 3, and checks what comes back against what the kernel promises a
program. they run on a booted kernel, the way any program does.

a selftest sees the kernel from where a program does. it is compiled with
``-nostdinc`` against ``include/uapi/`` and the compiler's freestanding
headers, and nothing else, so it can rely on the abi and on nothing behind
it. that is what makes the selftests the abi's own record, and why a
failing one is read the way `what a failing selftest means`_ says.

running the selftests
---------------------

to build the tests and run them:

.. code-block:: sh

   make kselftest

there is nothing to install. the tests are built with the image, which
carries them under ``selftests/``, so ``make kselftest`` builds whatever is
out of date, boots the image headless in qemu, logs in as guest, runs each
test with ``run``, and reads the results back off the serial port. qemu is
the one thing it needs beyond the build.

the results are one TAP 13 stream, each test's own TAP under it as ``#``
lines:

.. code-block:: text

   TAP version 13
   1..1
   # timeout set to 30
   # selftests: sched: wait
   # TAP version 13
   # 1..3
   # ok 1 exit(42) reaches the parent as 42
   # ok 2 a child ended by SIGTERM reaches the parent as 128 + 15
   # ok 3 wait refuses a pid that is not the caller's child
   # # Totals: pass:3 fail:0 xfail:0 xpass:0 skip:0 error:0
   ok 1 selftests: sched: wait
   # Totals: pass:1 fail:0

``make kselftest`` fails when any test does. the whole serial log of the
run, everything the kernel printed included, is kept in
``bin/velvet-kselftest.log``.

the tests run as guest, the account the kernel does the least for. a test
can do only what any logged-in user could, and a promise the kernel keeps
to a guest it keeps to everybody.

running a subset of selftests
-----------------------------

``TARGETS`` names the subsystems to run, by directory:

.. code-block:: sh

   make kselftest TARGETS=sched
   make kselftest TARGETS="sched x86_64"

without it every directory under ``tools/testing/selftests/`` is a target,
so the directories there are the list of targets.

the runner can also be called on its own, on any image, with the tests
named one by one:

.. code-block:: sh

   python3 tools/testing/selftests/run_kselftest.py velvet.img sched/wait
   python3 tools/testing/selftests/run_kselftest.py --timeout 60 velvet.img x86_64/fault

the machine has 2 cpus unless ``CPUS`` says otherwise:
``make kselftest CPUS=4``.

running a test by hand
----------------------

a selftest is an ordinary program on the medium, so it can be run at the
shell like any other. boot with ``make run``, log in, and:

.. code-block:: text

   run selftests/sched/wait

its TAP goes to the screen between whatever the kernel prints while it
runs, which is often the part worth reading. with ``&`` it runs in the
background, and ``ps`` and ``signal`` can look at it while it does.

timeouts and panics
-------------------

selftests are meant to be quick, a few seconds each, and every test gets
30. one that has not come back by then is reported as

.. code-block:: text

   not ok 1 selftests: x86_64: fault # TIMEOUT 30 seconds

and the run ends there: the shell is still waiting on that test, so
nothing after it can be run. ``--timeout`` gives the runner another limit
while a test is being worked on; for the tests in the tree it is 30.

a test that takes the kernel down is reported as ``# kernel panic``, with
the first lines of the panic under it, and every test after it as not run,
since there is no kernel left to run them on.

contributing new tests
----------------------

in general, the rules for selftests are:

- test what a program can see. a test includes ``kselftest.h`` and the
  uapi headers, the ``<velvet/...>`` ones under ``include/uapi/``, and
  nothing from inside the kernel.
- do everything as guest. the runner never logs in as anybody else.
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
- a test defines ``main`` and includes ``../kselftest.h``, which brings
  ``_start``. what ``main`` returns is the exit status, though a test ends
  with ``ksft_finished()``, which exits with the right one.
- it is freestanding: no libc, and built with ``-mno-sse``, so no floating
  point either. ``selftest.ld`` links it at 0x400000, where a program
  lives. gcc may still emit a call to ``memcpy`` or ``memset`` for a large
  initialiser and there is nothing for it to link against, so local arrays
  stay small.
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

the formats take ``%s``, ``%d``, ``%u`` and ``%x``, an ``l`` in front for
the 64-bit ones, and ``%%``. each line is written with one call, so it
reaches the serial port whole even when the kernel prints something at the
same moment.

``KSFT_PASS`` (0), ``KSFT_FAIL`` (1) and ``KSFT_SKIP`` (4) are the exit
codes. nothing skips yet.

the calls
~~~~~~~~~

each is one syscall and nothing more, so what a test exercises is the
kernel and not a library in front of it: ``write(fd, buf, len)``,
``exit(code)``, ``fork()``, ``wait(pid, &status)``, ``getpid()``,
``sigsend(pid, sig)``, ``sleep_ms(ms)``, ``open(path)``,
``read(fd, buf, len)``, ``close(fd)`` and ``readdir(path, index, buf,
size)``, which puts the name of a directory's entry ``index`` in ``buf``,
a directory's with a ``/`` after it, and returns -1 past the last.
``ksft_memeq(a, b, len)`` and ``ksft_streq(a, b)`` compare bytes and
strings, which a test without a libc would otherwise write each time.
``ksft_read_file(path, buf, size)`` reads a whole file, up to ``size``
bytes, and returns its length, or -1 when it would not open, and
``ksft_contains(text, len, needle)`` says whether ``needle`` is somewhere
in the first ``len`` bytes of ``text``: between them, a check of what a
file in ``/proc`` says.

``wait`` puts how the child ended in ``status``: what it passed to
``exit``, or 128 plus the signal that ended it. any other call is made
with ``ksft_syscall(nr, a0, a1, a2)`` and its number from
``<velvet/syscall.h>``, or ``ksft_syscall5`` for one that takes five.

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

   int main(int i_argc, char **pp_argv)
   {
       int i_status = -1;
       int i_pid;

       (void)i_argc;
       (void)pp_argv;
       ksft_print_header();
       ksft_set_plan(1);

       i_pid = fork();
       if (i_pid == 0) {
           exit(7);
       }
       if (i_pid > 0) {
           wait(i_pid, &i_status);
       }
       if (i_status != 7) {
           ksft_print_msg("wanted 7, read %d\n", i_status);
       }
       ksft_test_result(i_status == 7, "exit(7) reaches the parent as 7\n");
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
programs have to do about it, starting with velvetOS's.
